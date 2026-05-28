#include "AtlasSubsystem.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

#include "entitydef/entitydef_api.h"

#include "AtlasCoordinates.h"
#include "AtlasReconnectBackoff.h"
#include "AtlasUE.h"
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

	SpaceData = NewObject<UAtlasSpaceData>(this);
	NetClient->SetSpaceDataSink(SpaceData);

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

bool UAtlasSubsystem::BeginLogin(const FString& Host, int32 Port, const FString& Username,
                                 const FString& PasswordHash)
{
	if (!NetClient || Port <= 0 || Port > 0xFFFF) return false;
	const uint16 PortU16 = static_cast<uint16>(Port);
	// Cached so auto-reconnect can replay without re-prompting game code.
	CachedHost = Host;
	CachedPort = PortU16;
	CachedUsername = Username;
	CachedPasswordHash = PasswordHash;
	bHasCachedCredentials = true;
	// Manual BeginLogin is the only retry path after def_mismatch / reconnect
	// exhaust; clear every fatal-latch so the next attempt logs from a cold start.
	ReconnectAttempts = 0;
	bReconnectExhausted = false;
	bDefMismatchLogged = false;
	return NetClient->BeginLogin(Host, PortU16, Username, PasswordHash);
}

bool UAtlasSubsystem::BeginAuthenticate()
{
	return NetClient && NetClient->BeginAuthenticate();
}

bool UAtlasSubsystem::Logout()
{
	if (!NetClient || NetClient->GetContext() == nullptr) return false;
	bAutoReconnectEnabled = false;
	const int32 Result = AtlasNetDisconnect(NetClient->GetContext(), ATLAS_DISCONNECT_LOGOUT);
	return Result == ATLAS_NET_OK;
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

bool UAtlasSubsystem::SendMovementInput(atlas::EntityId EntityId,
                                        const AtlasMovementInputFrame* Frames, int32 FrameCount)
{
	if (!NetClient || NetClient->GetContext() == nullptr) return false;
	return AtlasNetSendMovementInput(NetClient->GetContext(), EntityId, Frames, FrameCount)
		== ATLAS_NET_OK;
}

bool UAtlasSubsystem::SendMovementCorrectionReport(atlas::EntityId EntityId,
                                                   uint32 AckedInputSeq, uint32 ServerTick,
                                                   float DistanceM, uint16 CorrectionFlags)
{
	if (!NetClient || NetClient->GetContext() == nullptr) return false;
	return AtlasNetSendMovementCorrectionReport(NetClient->GetContext(), EntityId, AckedInputSeq,
		ServerTick, DistanceM, CorrectionFlags) == ATLAS_NET_OK;
}

bool UAtlasSubsystem::SetTransportImpairment(int32 OneWayLatencyMs, int32 LossPermyriad,
                                             int32 Seed)
{
	if (!NetClient || NetClient->GetContext() == nullptr) return false;
	if (OneWayLatencyMs < 0 || LossPermyriad < 0 || LossPermyriad > 10000) return false;
	const uint32 SafeSeed = Seed > 0 ? static_cast<uint32>(Seed) : 1u;
	return AtlasNetSetTransportImpairment(NetClient->GetContext(),
		static_cast<uint32>(OneWayLatencyMs), static_cast<uint32>(LossPermyriad), SafeSeed)
		== ATLAS_NET_OK;
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

int32 UAtlasSubsystem::GetPlayerEntityId() const
{
	return NetClient ? static_cast<int32>(NetClient->GetPlayerEntityId()) : 0;
}

int32 UAtlasSubsystem::GetPlayerTypeId() const
{
	return NetClient ? static_cast<int32>(NetClient->GetPlayerTypeId()) : 0;
}

int32 UAtlasSubsystem::GetLastLoginStatus() const
{
	return NetClient ? static_cast<int32>(NetClient->GetLastLoginStatus()) : 0xFF;
}

FString UAtlasSubsystem::GetLastNetErrorMessage() const
{
	if (!NetClient || NetClient->GetContext() == nullptr) return FString();
	const char* Msg = AtlasNetLastError(NetClient->GetContext());
	return Msg ? FString(UTF8_TO_TCHAR(Msg)) : FString();
}

bool UAtlasSubsystem::GetNetStats(FAtlasNetStatsBp& OutStats) const
{
	OutStats = FAtlasNetStatsBp{};
	if (!NetClient || NetClient->GetContext() == nullptr) return false;
	AtlasNetStats Raw{};
	if (AtlasNetGetStats(NetClient->GetContext(), &Raw) != ATLAS_NET_OK) return false;
	OutStats.RttMs = static_cast<int32>(Raw.rtt_ms);
	OutStats.BytesSent = static_cast<int32>(Raw.bytes_sent);
	OutStats.BytesReceived = static_cast<int32>(Raw.bytes_recv);
	OutStats.PacketsLost = static_cast<int32>(Raw.packets_lost);
	OutStats.SendQueueSize = static_cast<int32>(Raw.send_queue_size);
	OutStats.LossRate = Raw.loss_rate;
	return true;
}

bool UAtlasSubsystem::OnTick(float DeltaTime)
{
	// Net state observed each tick so BP listeners only see real transitions.
	const EAtlasNetClientState ObservedState =
		NetClient ? NetClient->GetState() : EAtlasNetClientState::Idle;
	if (ObservedState != LastBroadcastNetState)
	{
		const EAtlasNetClientState PrevState = LastBroadcastNetState;
		LastBroadcastNetState = ObservedState;
		OnNetStateChanged.Broadcast(ObservedState);
		if (PrevState == EAtlasNetClientState::LoggingIn &&
			(ObservedState == EAtlasNetClientState::LoginSucceeded ||
			 ObservedState == EAtlasNetClientState::LoginFailed))
		{
			OnLoginFinished.Broadcast(NetClient
				? static_cast<int32>(NetClient->GetLastLoginStatus()) : 0xFF);
		}
	}

	// Null after a failed AttemptReconnect Create(); keep the backoff loop
	// alive so the subsystem isn't stuck on a single bad attempt.
	if (!NetClient)
	{
		if (!bReconnectExhausted && ReconnectAttempts >= AtlasReconnect::kMaxReconnectAttempts)
		{
			UE_LOG(LogAtlasSubsystem, Error,
				TEXT("AtlasNet reconnect exhausted after %d attempts (Create kept failing) — "
				     "auto-retry stopped"),
				ReconnectAttempts);
			bReconnectExhausted = true;
			OnReconnectExhausted.Broadcast();
		}
		if (bReconnectExhausted) return true;
		if (bAutoReconnectEnabled && bHasCachedCredentials)
		{
			const double Now = FPlatformTime::Seconds();
			if (NextReconnectAtSec == 0.0)
			{
				const double Wait = AtlasReconnect::ComputeBackoffSec(ReconnectAttempts);
				NextReconnectAtSec = Now + Wait;
				OnReconnectScheduled.Broadcast(ReconnectAttempts + 1, static_cast<float>(Wait));
			}
			else if (Now >= NextReconnectAtSec)
			{
				AttemptReconnect();
			}
		}
		return true;
	}

	switch (NetClient->GetState())
	{
		case EAtlasNetClientState::LoggingIn:
		case EAtlasNetClientState::Authenticating:
			NetClient->PollOnGameThread();
			break;
		case EAtlasNetClientState::Authenticated:
			if (!bRunningStarted)
			{
				// AuthenticateResult is the only owner-create signal on the wire;
				// without this the EntityManager misses the player and game RPCs idle.
				EntityManager.HandleCreate(NetClient->GetPlayerEntityId(),
				                           NetClient->GetPlayerTypeId());
				NetClient->StartRunningThread();
				bRunningStarted = true;
				ReconnectAttempts = 0;  // healthy session — restart backoff from 1s
			}
			break;
		case EAtlasNetClientState::Running:
			NetClient->TickGameThread(EntityManager);
			EntityManager.TickAll(DeltaTime);
			break;
		case EAtlasNetClientState::Disconnected:
		case EAtlasNetClientState::LoginFailed:
		case EAtlasNetClientState::AuthFailed:
			// def_mismatch is fatal: client/server entity-def schemas disagree,
			// retrying will keep failing and the backoff loop just burns cycles.
			if (NetClient && NetClient->GetLastLoginStatus() == ATLAS_LOGIN_DEF_MISMATCH)
			{
				if (!bDefMismatchLogged)
				{
					UE_LOG(LogAtlasSubsystem, Error,
						TEXT("AtlasNet def_mismatch — client/server entity-def out of sync; "
						     "auto-reconnect halted (client likely needs upgrade)"));
					bDefMismatchLogged = true;
				}
				break;
			}
			if (bReconnectExhausted) break;
			if (ReconnectAttempts >= AtlasReconnect::kMaxReconnectAttempts)
			{
				UE_LOG(LogAtlasSubsystem, Error,
					TEXT("AtlasNet reconnect exhausted after %d attempts — auto-retry stopped; "
					     "game code can call BeginLogin to retry manually"),
					ReconnectAttempts);
				bReconnectExhausted = true;
				OnReconnectExhausted.Broadcast();
				break;
			}
			if (bAutoReconnectEnabled && bHasCachedCredentials)
			{
				const double Now = FPlatformTime::Seconds();
				if (NextReconnectAtSec == 0.0)
				{
					const double Wait = AtlasReconnect::ComputeBackoffSec(ReconnectAttempts);
					NextReconnectAtSec = Now + Wait;
					UE_LOG(LogAtlasSubsystem, Log,
						TEXT("AtlasNet disconnected; next reconnect attempt in %.1f s (attempt #%d)"),
						Wait, ReconnectAttempts + 1);
					OnReconnectScheduled.Broadcast(ReconnectAttempts + 1, static_cast<float>(Wait));
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

	// Stale entities still route through the dead net ctx; their actors
	// cascade-destroy via ~FAtlasUEActorView.
	EntityManager.Clear();
	if (SpaceData != nullptr) SpaceData->Clear();
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
	if (SpaceData != nullptr) NetClient->SetSpaceDataSink(SpaceData);

	if (!NetClient->BeginLogin(CachedHost, CachedPort, CachedUsername, CachedPasswordHash))
	{
		UE_LOG(LogAtlasSubsystem, Warning,
			TEXT("BeginLogin failed during reconnect; net stack will surface Disconnected"));
	}
}
