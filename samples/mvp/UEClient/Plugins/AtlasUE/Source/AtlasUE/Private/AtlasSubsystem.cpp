#include "AtlasSubsystem.h"

#include "Engine/World.h"
#include "Logging/LogMacros.h"

#include "AtlasCoordinates.h"
#include "AtlasUEActorView.h"

DEFINE_LOG_CATEGORY_STATIC(LogAtlasSubsystem, Log, All);

void UAtlasSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	NetClient = MakeUnique<FAtlasNetClient>();
	if (!NetClient->Create())
	{
		UE_LOG(LogAtlasSubsystem, Error, TEXT("FAtlasNetClient::Create failed"));
		NetClient.Reset();
		return;
	}

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UAtlasSubsystem::OnTick));
}

void UAtlasSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (NetClient)
	{
		NetClient->Destroy();
		NetClient.Reset();
	}
	EntityManager.Clear();
	TypeRegistry.Empty();
	bRunningStarted = false;

	Super::Deinitialize();
}

bool UAtlasSubsystem::BeginLogin(const FString& Host, uint16 Port, const FString& Username,
                                 const FString& PasswordHash)
{
	return NetClient && NetClient->BeginLogin(Host, Port, Username, PasswordHash);
}

bool UAtlasSubsystem::BeginAuthenticate()
{
	return NetClient && NetClient->BeginAuthenticate();
}

void UAtlasSubsystem::RegisterEntityClass(uint16 TypeId, TSubclassOf<AActor> ActorClass,
                                          EntityFactory Factory)
{
	TypeRegistry.Add(TypeId, FTypeReg{ActorClass, MoveTemp(Factory)});

	EntityManager.RegisterFactory(TypeId, [this, TypeId](atlas::EntityId Id,
	                                                    atlas::EntityTypeId /*Type*/) {
		return InstantiateEntity(TypeId, Id);
	});
}

std::unique_ptr<atlas::ClientEntity> UAtlasSubsystem::InstantiateEntity(uint16 TypeId,
                                                                       atlas::EntityId Id)
{
	const FTypeReg* Reg = TypeRegistry.Find(TypeId);
	if (!Reg) return nullptr;

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogAtlasSubsystem, Warning,
			TEXT("InstantiateEntity %u type=%u: no UWorld, skipping spawn"), Id, TypeId);
		return nullptr;
	}

	AActor* Actor = World->SpawnActor<AActor>(Reg->ActorClass);
	if (!Actor)
	{
		UE_LOG(LogAtlasSubsystem, Warning,
			TEXT("InstantiateEntity %u type=%u: SpawnActor failed"), Id, TypeId);
		return nullptr;
	}

	std::unique_ptr<atlas::ClientEntity> Entity;
	if (Reg->Factory)
	{
		Entity = Reg->Factory(Id, static_cast<atlas::EntityTypeId>(TypeId));
	}
	else
	{
		Entity = std::make_unique<atlas::ClientEntity>(Id, static_cast<atlas::EntityTypeId>(TypeId));
	}
	if (!Entity)
	{
		Actor->Destroy();
		return nullptr;
	}

	Entity->AttachView(std::make_unique<FAtlasUEActorView>(Actor));
	return Entity;
}

EAtlasNetClientState UAtlasSubsystem::GetNetState() const
{
	return NetClient ? NetClient->GetState() : EAtlasNetClientState::Idle;
}

uint32 UAtlasSubsystem::GetPlayerEntityId() const
{
	return NetClient ? NetClient->GetPlayerEntityId() : 0;
}

uint16 UAtlasSubsystem::GetPlayerTypeId() const
{
	return NetClient ? NetClient->GetPlayerTypeId() : 0;
}

bool UAtlasSubsystem::OnTick(float DeltaTime)
{
	if (!NetClient) return true;

	switch (NetClient->GetState())
	{
		case EAtlasNetClientState::LoggingIn:
		case EAtlasNetClientState::Authenticating:
			NetClient->PollOnGameThread();
			break;
		case EAtlasNetClientState::Authenticated:
			if (!bRunningStarted)
			{
				NetClient->StartRunningThread();
				bRunningStarted = true;
			}
			break;
		case EAtlasNetClientState::Running:
			NetClient->TickGameThread(EntityManager);
			EntityManager.TickAll(DeltaTime);
			break;
		default:
			// Idle / LoginSucceeded / LoginFailed / AuthFailed / Disconnected are
			// game-side waypoints; the subsystem doesn't auto-advance them.
			break;
	}
	return true;
}
