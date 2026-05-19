#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/SubclassOf.h"

#include <functional>
#include <memory>

#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/rpc_sender.h"
#include "AtlasNetClient.h"
#include "AtlasSpaceData.h"

#include "AtlasSubsystem.generated.h"

// BP-facing events; net thread → game thread happens inside FAtlasNetClient,
// so handlers always fire on the GameThread tick.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAtlasOnNetStateChanged,
	EAtlasNetClientState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAtlasOnLoginFinished,
	int32, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAtlasOnReconnectScheduled,
	int32, AttemptNumber, float, NextRetrySec);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAtlasOnReconnectExhausted);

// BP-facing mirror of AtlasNetStats with int32 fields. Filled by
// UAtlasSubsystem::GetNetStats; zeroed when the net client isn't live yet.
USTRUCT(BlueprintType)
struct FAtlasNetStatsBp
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Atlas|Net")
	int32 RttMs = 0;

	UPROPERTY(BlueprintReadOnly, Category="Atlas|Net")
	int32 BytesSent = 0;

	UPROPERTY(BlueprintReadOnly, Category="Atlas|Net")
	int32 BytesReceived = 0;

	UPROPERTY(BlueprintReadOnly, Category="Atlas|Net")
	int32 PacketsLost = 0;

	UPROPERTY(BlueprintReadOnly, Category="Atlas|Net")
	int32 SendQueueSize = 0;

	UPROPERTY(BlueprintReadOnly, Category="Atlas|Net")
	float LossRate = 0.f;
};

UCLASS()
class ATLASUE_API UAtlasSubsystem : public UGameInstanceSubsystem, public atlas::RpcSender
{
	GENERATED_BODY()

public:
	using EntityFactory = std::function<std::unique_ptr<atlas::ClientEntity>(
		atlas::EntityId, atlas::EntityTypeId)>;

	// Fires after entity + FAtlasUEActorView are wired so game code can link
	// engine-side bridges (e.g., UAtlasAvatarView) to the new entity.
	using EntityPostBind = std::function<void(atlas::ClientEntity*, AActor*)>;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category="Atlas|Net")
	bool BeginLogin(const FString& Host, int32 Port, const FString& Username,
	                const FString& PasswordHash);

	UFUNCTION(BlueprintCallable, Category="Atlas|Net")
	bool BeginAuthenticate();

	// Clean shutdown: hands ATLAS_DISCONNECT_LOGOUT to the SDK (so the server
	// releases the Account proxy) and clears auto-reconnect — the on-disconnect
	// tick would otherwise immediately reconnect from cached credentials.
	UFUNCTION(BlueprintCallable, Category="Atlas|Net")
	bool Logout();

	// False to suppress auto-retry (user-driven logout / future LoginScreen).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Atlas|Reconnect")
	bool bAutoReconnectEnabled = true;

	// 32-byte SHA-256 of the entity_defs surface; must be called before
	// BeginLogin or the server rejects with def_mismatch.
	bool SetEntityDefDigest(TArrayView<const uint8> Digest);

	// atlas::RpcSender — codegen-emitted entity stubs route here. Both deliver
	// straight through to AtlasNetSend{Base,Cell}Rpc once the net client is up.
	virtual void SendBaseRpc(atlas::EntityId Id, uint32 RpcId, const uint8* Args,
		int32 ArgsLen) override;
	virtual void SendCellRpc(atlas::EntityId Id, uint32 RpcId, const uint8* Args,
		int32 ArgsLen) override;

	// Factory null → base ClientEntity. PostBind runs after AttachView.
	void RegisterEntityClass(uint16 TypeId, TSubclassOf<AActor> ActorClass,
	                         EntityFactory Factory = nullptr,
	                         EntityPostBind PostBind = nullptr);

	UFUNCTION(BlueprintPure, Category="Atlas|Net")
	EAtlasNetClientState GetNetState() const;

	UFUNCTION(BlueprintPure, Category="Atlas|Net")
	int32 GetPlayerEntityId() const;

	UFUNCTION(BlueprintPure, Category="Atlas|Net")
	int32 GetPlayerTypeId() const;

	// AtlasLoginStatus enum from the last LoginResult; 0xFF before any attempt.
	UFUNCTION(BlueprintPure, Category="Atlas|Net")
	int32 GetLastLoginStatus() const;

	// AtlasNetLastError text — populated whenever a Login / Authenticate or
	// internal step fails; empty when the session is healthy.
	UFUNCTION(BlueprintPure, Category="Atlas|Net")
	FString GetLastNetErrorMessage() const;

	// AtlasNetGetStats snapshot; returns true when the net ctx is live and
	// the SDK populated stats this call.
	UFUNCTION(BlueprintPure, Category="Atlas|Net")
	bool GetNetStats(FAtlasNetStatsBp& OutStats) const;

	[[nodiscard]] atlas::ClientEntityManager& GetEntityManager() { return EntityManager; }

	UFUNCTION(BlueprintPure, Category="Atlas|SpaceData")
	UAtlasSpaceData* GetSpaceData() const { return SpaceData; }

	UPROPERTY(BlueprintAssignable, Category="Atlas|Net")
	FAtlasOnNetStateChanged OnNetStateChanged;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Net")
	FAtlasOnLoginFinished OnLoginFinished;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Reconnect")
	FAtlasOnReconnectScheduled OnReconnectScheduled;

	UPROPERTY(BlueprintAssignable, Category="Atlas|Reconnect")
	FAtlasOnReconnectExhausted OnReconnectExhausted;

private:
	bool OnTick(float DeltaTime);
	void AttemptReconnect();

	std::unique_ptr<atlas::ClientEntity> InstantiateEntity(uint16 TypeId,
	                                                      atlas::EntityId Id);

	TUniquePtr<FAtlasNetClient> NetClient;
	atlas::ClientEntityManager EntityManager;

	UPROPERTY(Transient)
	UAtlasSpaceData* SpaceData = nullptr;

	struct FTypeReg
	{
		TSubclassOf<AActor> ActorClass;
		EntityFactory Factory;
		EntityPostBind PostBind;
	};
	TMap<uint16, FTypeReg> TypeRegistry;

	FTSTicker::FDelegateHandle TickHandle;
	bool bRunningStarted = false;

	// Captured on first BeginLogin; replayed by auto-reconnect.
	FString CachedHost;
	uint16 CachedPort = 0;
	FString CachedUsername;
	FString CachedPasswordHash;
	bool bHasCachedCredentials = false;

	int32 ReconnectAttempts = 0;
	double NextReconnectAtSec = 0.0;
	// Latches once we've surfaced the def_mismatch warning so the log isn't
	// repeated each tick while auto-reconnect is suppressed.
	bool bDefMismatchLogged = false;
	// True once ReconnectAttempts has hit kMaxReconnectAttempts; cleared only
	// by a fresh BeginLogin() so game code can drive a manual retry.
	bool bReconnectExhausted = false;
	// Tick-local cache so OnNetStateChanged only fires on actual transitions.
	EAtlasNetClientState LastBroadcastNetState = EAtlasNetClientState::Idle;
};
