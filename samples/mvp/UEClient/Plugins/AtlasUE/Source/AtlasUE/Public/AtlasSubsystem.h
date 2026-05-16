#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GameFramework/Actor.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/SubclassOf.h"

#include <functional>
#include <memory>

#include "AtlasCore/client_entity_manager.h"
#include "AtlasNetClient.h"

#include "AtlasSubsystem.generated.h"

UCLASS()
class ATLASUE_API UAtlasSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	using EntityFactory = std::function<std::unique_ptr<atlas::ClientEntity>(
		atlas::EntityId, atlas::EntityTypeId)>;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool BeginLogin(const FString& Host, uint16 Port, const FString& Username,
	                const FString& PasswordHash);
	bool BeginAuthenticate();

	// 32-byte SHA-256 of the entity_defs surface; must be called before
	// BeginLogin or the server rejects with def_mismatch.
	bool SetEntityDefDigest(TArrayView<const uint8> Digest);

	// Pass-through to AtlasNetSend{Base,Cell}Rpc. Caller owns the rpc_id and
	// payload byte layout until typed stubs are generated.
	bool SendBaseRpc(uint32 EntityId, uint32 RpcId, const TArray<uint8>& Payload);
	bool SendCellRpc(uint32 EntityId, uint32 RpcId, const TArray<uint8>& Payload);

	// On HandleEnter the subsystem spawns the bound Actor, runs Factory (or the
	// base ClientEntity if null), and attaches an FAtlasUEActorView.
	void RegisterEntityClass(uint16 TypeId, TSubclassOf<AActor> ActorClass,
	                         EntityFactory Factory = nullptr);

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
	};
	TMap<uint16, FTypeReg> TypeRegistry;

	FTSTicker::FDelegateHandle TickHandle;
	bool bRunningStarted = false;
};
