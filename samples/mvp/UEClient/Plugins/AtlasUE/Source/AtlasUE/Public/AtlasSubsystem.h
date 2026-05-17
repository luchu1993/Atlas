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

	// Runs after the entity is created and the FAtlasUEActorView is attached;
	// game code uses this to wire engine-side bridges (e.g., UAtlasAvatarView)
	// to the typed entity instance the factory produced.
	using EntityPostBind = std::function<void(atlas::ClientEntity*, AActor*)>;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool BeginLogin(const FString& Host, uint16 Port, const FString& Username,
	                const FString& PasswordHash);
	bool BeginAuthenticate();

	// 32-byte SHA-256 of the entity_defs surface; must be called before
	// BeginLogin or the server rejects with def_mismatch.
	bool SetEntityDefDigest(TArrayView<const uint8> Digest);

	// atlas::RpcSender — codegen-emitted entity stubs route here. Both deliver
	// straight through to AtlasNetSend{Base,Cell}Rpc once the net client is up.
	virtual void SendBaseRpc(atlas::EntityId Id, uint32 RpcId, const uint8* Args,
		int32 ArgsLen) override;
	virtual void SendCellRpc(atlas::EntityId Id, uint32 RpcId, const uint8* Args,
		int32 ArgsLen) override;

	// On HandleEnter the subsystem spawns the bound Actor, runs Factory (or the
	// base ClientEntity if null), attaches an FAtlasUEActorView, then invokes
	// PostBind so game code can link engine-side bridge components to the
	// freshly-created entity.
	void RegisterEntityClass(uint16 TypeId, TSubclassOf<AActor> ActorClass,
	                         EntityFactory Factory = nullptr,
	                         EntityPostBind PostBind = nullptr);

	[[nodiscard]] EAtlasNetClientState GetNetState() const;
	[[nodiscard]] uint32 GetPlayerEntityId() const;
	[[nodiscard]] uint16 GetPlayerTypeId() const;

	[[nodiscard]] atlas::ClientEntityManager& GetEntityManager() { return EntityManager; }

private:
	bool OnTick(float DeltaTime);

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
};
