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

	// Registered before BeginLogin (typically GameMode::BeginPlay). On HandleEnter
	// the subsystem spawns the bound Actor on the GameThread, runs the optional
	// custom EntityFactory (else atlas::ClientEntity base), and attaches a fresh
	// FAtlasUEActorView. A null Factory works for entities that do not need
	// AvatarFilter / typed state.
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
