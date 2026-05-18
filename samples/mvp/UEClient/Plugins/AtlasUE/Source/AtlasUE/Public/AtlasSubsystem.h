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

#include "AtlasSubsystem.generated.h"

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

	bool BeginLogin(const FString& Host, uint16 Port, const FString& Username,
	                const FString& PasswordHash);
	bool BeginAuthenticate();

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

	[[nodiscard]] EAtlasNetClientState GetNetState() const;
	[[nodiscard]] uint32 GetPlayerEntityId() const;
	[[nodiscard]] uint16 GetPlayerTypeId() const;
	// AtlasLoginStatus from the last LoginResult; 0xFF before any attempt.
	[[nodiscard]] uint8 GetLastLoginStatus() const;

	[[nodiscard]] atlas::ClientEntityManager& GetEntityManager() { return EntityManager; }

private:
	bool OnTick(float DeltaTime);
	void AttemptReconnect();

	std::unique_ptr<atlas::ClientEntity> InstantiateEntity(uint16 TypeId,
	                                                      atlas::EntityId Id);

	TUniquePtr<FAtlasNetClient> NetClient;
	atlas::ClientEntityManager EntityManager;

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
};
