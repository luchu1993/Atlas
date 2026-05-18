#include "AtlasSubsystem.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Math/UnrealMathUtility.h"

#include "entitydef/entitydef_api.h"

#include "AtlasCoordinates.h"
#include "AtlasUE.h"
#include "AtlasUEActorView.h"

DEFINE_LOG_CATEGORY_STATIC(LogAtlasSubsystem, Log, All);

namespace
{
// 1s, 2s, 4s, ..., capped at 30s. First attempt fires after kBaseBackoffSec
// from disconnect (no immediate hammer on the server).
constexpr double kBaseBackoffSec = 1.0;
constexpr double kMaxBackoffSec = 30.0;

double ComputeBackoffSec(int32 Attempts)
{
	const double Scaled = kBaseBackoffSec * FMath::Pow(2.0, static_cast<double>(Attempts));
	return FMath::Min(Scaled, kMaxBackoffSec);
}
}  // namespace

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

	if (AtlasEdrContext* Edr = FAtlasUEModule::GetEdrContext())
	{
		EntityManager.SetDescriptorContext(Edr);
		const uint8* Digest = AtlasEdrGetDigest(Edr);
		const int32 Size = AtlasEdrGetDigestSize(Edr);
		if (Digest != nullptr && Size > 0)
		{
			SetEntityDefDigest(TArrayView<const uint8>(Digest, Size));
		}
	}
	EntityManager.SetRpcSender(this);

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
	if (!NetClient) return false;
	// Cache for auto-reconnect. Game code only has to supply creds once;
	// the subsystem replays them on disconnect.
	CachedHost = Host;
	CachedPort = Port;
	CachedUsername = Username;
	CachedPasswordHash = PasswordHash;
	bHasCachedCredentials = true;
	return NetClient->BeginLogin(Host, Port, Username, PasswordHash);
}

bool UAtlasSubsystem::BeginAuthenticate()
{
	return NetClient && NetClient->BeginAuthenticate();
}

bool UAtlasSubsystem::SetEntityDefDigest(TArrayView<const uint8> Digest)
{
	if (!NetClient || NetClient->GetContext() == nullptr) return false;
	return AtlasNetSetEntityDefDigest(NetClient->GetContext(), Digest.GetData(),
		Digest.Num()) == ATLAS_NET_OK;
}

void UAtlasSubsystem::SendBaseRpc(atlas::EntityId Id, uint32 RpcId, const uint8* Args,
	int32 ArgsLen)
{
	if (!NetClient || NetClient->GetContext() == nullptr) return;
	AtlasNetSendBaseRpc(NetClient->GetContext(), Id, RpcId, Args, ArgsLen);
}

void UAtlasSubsystem::SendCellRpc(atlas::EntityId Id, uint32 RpcId, const uint8* Args,
	int32 ArgsLen)
{
	if (!NetClient || NetClient->GetContext() == nullptr) return;
	AtlasNetSendCellRpc(NetClient->GetContext(), Id, RpcId, Args, ArgsLen);
}

void UAtlasSubsystem::RegisterEntityClass(uint16 TypeId, TSubclassOf<AActor> ActorClass,
                                          EntityFactory Factory, EntityPostBind PostBind)
{
	TypeRegistry.Add(TypeId, FTypeReg{ActorClass, MoveTemp(Factory), MoveTemp(PostBind)});

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
	UE_LOG(LogAtlasSubsystem, Log,
		TEXT("Spawned %s for eid=%u type=%u at UE pos=(%.1f, %.1f, %.1f)"),
		*Actor->GetClass()->GetName(), Id, TypeId,
		Actor->GetActorLocation().X, Actor->GetActorLocation().Y, Actor->GetActorLocation().Z);

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
	if (Reg->PostBind) Reg->PostBind(Entity.get(), Actor);
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
				// First Running-bound transition; reset backoff so the next
				// disconnect starts from 1 s again.
				ReconnectAttempts = 0;
			}
			break;
		case EAtlasNetClientState::Running:
			NetClient->TickGameThread(EntityManager);
			EntityManager.TickAll(DeltaTime);
			break;
		case EAtlasNetClientState::Disconnected:
		case EAtlasNetClientState::LoginFailed:
		case EAtlasNetClientState::AuthFailed:
			if (bAutoReconnectEnabled && bHasCachedCredentials)
			{
				const double Now = FPlatformTime::Seconds();
				if (NextReconnectAtSec == 0.0)
				{
					NextReconnectAtSec = Now + ComputeBackoffSec(ReconnectAttempts);
					UE_LOG(LogAtlasSubsystem, Log,
						TEXT("AtlasNet disconnected; next reconnect attempt in %.1f s (attempt #%d)"),
						NextReconnectAtSec - Now, ReconnectAttempts + 1);
				}
				else if (Now >= NextReconnectAtSec)
				{
					AttemptReconnect();
				}
			}
			break;
		default:
			break;
	}
	return true;
}

void UAtlasSubsystem::AttemptReconnect()
{
	++ReconnectAttempts;
	NextReconnectAtSec = 0.0;
	UE_LOG(LogAtlasSubsystem, Log, TEXT("AtlasNet reconnect attempt #%d"), ReconnectAttempts);

	// Stale entities (Account, peers from old session) carry the dead
	// net ctx as their RpcSender route — drop them so codegen stubs don't
	// fire into the void. Actor destruction cascades via FAtlasUEActorView.
	EntityManager.Clear();
	bRunningStarted = false;

	if (NetClient)
	{
		NetClient->Destroy();
	}
	NetClient = MakeUnique<FAtlasNetClient>();
	if (!NetClient->Create())
	{
		UE_LOG(LogAtlasSubsystem, Error,
			TEXT("FAtlasNetClient::Create failed during reconnect; will retry on next tick"));
		NetClient.Reset();
		return;
	}

	// Restore the descriptor context + digest + sender on the fresh ctx.
	if (AtlasEdrContext* Edr = FAtlasUEModule::GetEdrContext())
	{
		EntityManager.SetDescriptorContext(Edr);
		const uint8* Digest = AtlasEdrGetDigest(Edr);
		const int32 Size = AtlasEdrGetDigestSize(Edr);
		if (Digest != nullptr && Size > 0)
		{
			SetEntityDefDigest(TArrayView<const uint8>(Digest, Size));
		}
	}
	EntityManager.SetRpcSender(this);

	if (!NetClient->BeginLogin(CachedHost, CachedPort, CachedUsername, CachedPasswordHash))
	{
		UE_LOG(LogAtlasSubsystem, Warning,
			TEXT("BeginLogin failed during reconnect; net stack will surface Disconnected"));
	}
}
