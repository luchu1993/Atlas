#include "AtlasNetClient.h"

#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Logging/LogMacros.h"

#include "AtlasCore/aoi_envelope.h"
#include "AtlasCore/client_entity.h"
#include "AtlasCore/client_entity_manager.h"
#include "AtlasCore/span_reader.h"

DEFINE_LOG_CATEGORY_STATIC(LogAtlasNet, Log, All);

// Internal helper — never exposed beyond this translation unit. FAtlasNetClient
// holds a pointer via the forward declaration in the header.
class FAtlasNetRunnable : public FRunnable
{
public:
	explicit FAtlasNetRunnable(AtlasNetContext* InCtx) : Ctx(InCtx) {}

	bool Init() override { return true; }
	uint32 Run() override
	{
		while (!bStop.load(std::memory_order_acquire))
		{
			AtlasNetPoll(Ctx);
			FPlatformProcess::Sleep(0.005f);
		}
		return 0;
	}
	void Stop() override { bStop.store(true, std::memory_order_release); }
	void Exit() override {}

private:
	AtlasNetContext* Ctx = nullptr;
	std::atomic<bool> bStop{false};
};

FCriticalSection FAtlasNetClient::ContextRegistryMutex;
TMap<AtlasNetContext*, FAtlasNetClient*> FAtlasNetClient::ContextRegistry;

FAtlasNetClient::FAtlasNetClient() = default;

FAtlasNetClient::~FAtlasNetClient()
{
	Destroy();
}

bool FAtlasNetClient::Create()
{
	if (Ctx != nullptr) return true;

	Ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
	if (Ctx == nullptr)
	{
		const char* Err = AtlasNetGlobalLastError();
		UE_LOG(LogAtlasNet, Error, TEXT("AtlasNetCreate failed: %s"),
			Err ? ANSI_TO_TCHAR(Err) : TEXT("(null)"));
		return false;
	}

	{
		FScopeLock Lock(&ContextRegistryMutex);
		ContextRegistry.Add(Ctx, this);
	}

	AtlasNetCallbacks Callbacks{};
	Callbacks.on_disconnect = &FAtlasNetClient::OnDisconnectStatic;
	Callbacks.on_deliver = &FAtlasNetClient::OnDeliverStatic;
	const int32 Result = AtlasNetSetCallbacks(Ctx, &Callbacks);
	if (Result != ATLAS_NET_OK)
	{
		UE_LOG(LogAtlasNet, Error, TEXT("AtlasNetSetCallbacks failed: %d"), Result);
		Destroy();
		return false;
	}

	return true;
}

void FAtlasNetClient::Destroy()
{
	if (Runnable && Thread)
	{
		Runnable->Stop();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
		delete Runnable;
		Runnable = nullptr;
	}

	if (Ctx != nullptr)
	{
		{
			FScopeLock Lock(&ContextRegistryMutex);
			ContextRegistry.Remove(Ctx);
		}
		AtlasNetDestroy(Ctx);
		Ctx = nullptr;
	}

	// Net thread is joined above (WaitForCompletion + delete Thread); these
	// stores are race-free, so relaxed is enough — no observer is in flight.
	FAtlasInboundMessage Drain;
	while (Inbound.Dequeue(Drain)) {}
	InboundDepth.store(0, std::memory_order_relaxed);
	bInboundOverflowLogged.store(false, std::memory_order_relaxed);
	PlayerEntityId.store(0, std::memory_order_relaxed);
	PlayerTypeId.store(0, std::memory_order_relaxed);
	LastLoginStatus.store(0xFF, std::memory_order_relaxed);
	State.store(EAtlasNetClientState::Idle, std::memory_order_relaxed);
}

bool FAtlasNetClient::BeginLogin(const FString& Host, uint16 Port, const FString& Username,
                                 const FString& PasswordHash)
{
	if (Ctx == nullptr) return false;
	const auto HostBytes = StringCast<ANSICHAR>(*Host);
	const auto UserBytes = StringCast<ANSICHAR>(*Username);
	const auto PwdBytes = StringCast<ANSICHAR>(*PasswordHash);
	const int32 Result = AtlasNetLogin(Ctx, HostBytes.Get(), Port, UserBytes.Get(),
		PwdBytes.Get(), &FAtlasNetClient::OnLoginResultStatic, this);
	if (Result != ATLAS_NET_OK)
	{
		UE_LOG(LogAtlasNet, Error, TEXT("AtlasNetLogin failed: %d"), Result);
		State.store(EAtlasNetClientState::LoginFailed);
		return false;
	}
	State.store(EAtlasNetClientState::LoggingIn);
	return true;
}

bool FAtlasNetClient::BeginAuthenticate()
{
	if (Ctx == nullptr) return false;
	const int32 Result = AtlasNetAuthenticate(Ctx, &FAtlasNetClient::OnAuthResultStatic, this);
	if (Result != ATLAS_NET_OK)
	{
		UE_LOG(LogAtlasNet, Error, TEXT("AtlasNetAuthenticate failed: %d"), Result);
		State.store(EAtlasNetClientState::AuthFailed);
		return false;
	}
	State.store(EAtlasNetClientState::Authenticating);
	return true;
}

void FAtlasNetClient::PollOnGameThread()
{
	if (Ctx != nullptr)
	{
		AtlasNetPoll(Ctx);
	}
}

void FAtlasNetClient::StartRunningThread()
{
	if (Thread != nullptr || Ctx == nullptr) return;
	Runnable = new FAtlasNetRunnable(Ctx);
	Thread = FRunnableThread::Create(Runnable, TEXT("AtlasNetClient"));
	State.store(EAtlasNetClientState::Running);
}

void FAtlasNetClient::TickGameThread(atlas::ClientEntityManager& Manager)
{
	FAtlasInboundMessage Msg;
	while (Inbound.Dequeue(Msg))
	{
		InboundDepth.fetch_sub(1, std::memory_order_relaxed);
		if (Msg.MsgId == kAtlasInboundDisconnectSentinel)
		{
			int32 Reason = 0;
			if (Msg.Payload.Num() >= static_cast<int32>(sizeof(int32)))
			{
				FMemory::Memcpy(&Reason, Msg.Payload.GetData(), sizeof(int32));
			}
			UE_LOG(LogAtlasNet, Log, TEXT("AtlasNet disconnect reason=%d"), Reason);
			State.store(EAtlasNetClientState::Disconnected);
			// Everything still queued is from the dead session — drop it so the
			// next session starts clean.
			while (Inbound.Dequeue(Msg))
			{
				InboundDepth.fetch_sub(1, std::memory_order_relaxed);
			}
			return;
		}
		if (Msg.MsgId == 0xF001 || Msg.MsgId == 0xF003)
		{
			// Lightweight Enter/Leave trace before delegating to the real decoder
			// below. Hard offsets only feed UE_LOG — if the wire schema changes
			// these print stale values but actual entity state still goes through
			// DecodeAoIEnvelope, so this block can't corrupt game state.
			if (Msg.Payload.Num() >= 5)
			{
				const uint8 Kind = Msg.Payload[0];
				uint32 EntityId = 0;
				FMemory::Memcpy(&EntityId, Msg.Payload.GetData() + 1, 4);
				if (Kind == 1 && Msg.Payload.Num() >= 5 + 35)
				{
					uint16 TypeId = 0;
					float Px = 0.f, Py = 0.f, Pz = 0.f;
					FMemory::Memcpy(&TypeId, Msg.Payload.GetData() + 5, 2);
					FMemory::Memcpy(&Px, Msg.Payload.GetData() + 7, 4);
					FMemory::Memcpy(&Py, Msg.Payload.GetData() + 11, 4);
					FMemory::Memcpy(&Pz, Msg.Payload.GetData() + 15, 4);
					UE_LOG(LogAtlasNet, Log,
						TEXT("AoI Enter eid=%u type=%u pos=(%.2f, %.2f, %.2f)"),
						EntityId, TypeId, Px, Py, Pz);
				}
				else if (Kind == 2)
				{
					UE_LOG(LogAtlasNet, Log, TEXT("AoI Leave eid=%u"), EntityId);
				}
				// kind=3 / kind=4 are per-tick high frequency — silent dispatch.
			}
			const auto Result = atlas::DecodeAoIEnvelope(
				Msg.Payload.GetData(), Msg.Payload.Num(), Manager);
			if (Result != atlas::EnvelopeDecodeResult::kOk)
			{
				UE_LOG(LogAtlasNet, Warning,
					TEXT("AoI decode result=%d msg_id=0x%04X"), (int32)Result, Msg.MsgId);
			}
		}
		else if (Msg.MsgId == 0xF004)
		{
			// ClientRpcEnvelope: [u32 entity_id][u32 rpc_id][u64 trace_id][args]
			if (Msg.Payload.Num() < 16)
			{
				UE_LOG(LogAtlasNet, Warning,
					TEXT("Client RPC truncated header len=%d"), Msg.Payload.Num());
				continue;
			}
			uint32 EntityId = 0;
			uint32 RpcId = 0;
			uint64 TraceId = 0;
			FMemory::Memcpy(&EntityId, Msg.Payload.GetData(), 4);
			FMemory::Memcpy(&RpcId, Msg.Payload.GetData() + 4, 4);
			FMemory::Memcpy(&TraceId, Msg.Payload.GetData() + 8, 8);
			if (atlas::ClientEntity* Entity = Manager.Find(EntityId))
			{
				atlas::SpanReader R(Msg.Payload.GetData() + 16,
					static_cast<std::size_t>(Msg.Payload.Num() - 16));
				if (!Entity->DispatchRpc(RpcId, TraceId, R))
				{
					UE_LOG(LogAtlasNet, Warning,
						TEXT("Client RPC unhandled rpc_id=0x%08X eid=%u"), RpcId, EntityId);
				}
			}
			// Unknown entity_id: drop (server may target an entity we just lost).
		}
		else if (Msg.MsgId == 2024 && Msg.Payload.Num() >= 6)
		{
			// kEntityTransferred (baseapp_messages.h): [u32 entity_id][u16 type_id].
			// Bump PlayerEntityId so "my current entity" follows the handoff.
			uint32 NewEntityId = 0;
			uint16 NewTypeId = 0;
			FMemory::Memcpy(&NewEntityId, Msg.Payload.GetData(), 4);
			FMemory::Memcpy(&NewTypeId, Msg.Payload.GetData() + 4, 2);
			const bool Created = Manager.HandleCreate(NewEntityId, NewTypeId);
			PlayerEntityId.store(NewEntityId);
			PlayerTypeId.store(NewTypeId);
			UE_LOG(LogAtlasNet, Log,
				TEXT("EntityTransferred eid=%u type=%u created=%d (now owner)"),
				NewEntityId, NewTypeId, Created ? 1 : 0);
		}
		else
		{
			UE_LOG(LogAtlasNet, Verbose,
				TEXT("unhandled msg_id=0x%04X len=%d"), Msg.MsgId, Msg.Payload.Num());
		}
	}
}

FAtlasNetClient* FAtlasNetClient::FindByCtx(AtlasNetContext* Ctx)
{
	FScopeLock Lock(&ContextRegistryMutex);
	FAtlasNetClient** Found = ContextRegistry.Find(Ctx);
	return Found ? *Found : nullptr;
}

void FAtlasNetClient::OnLoginResultStatic(void* UserData, uint8 Status, const char* /*Host*/,
                                          uint16 /*Port*/, const char* ErrorMessage)
{
	if (UserData == nullptr) return;
	FAtlasNetClient* Self = static_cast<FAtlasNetClient*>(UserData);
	// Status MUST land before State so a GetState()=LoginFailed observer
	// reading LastLoginStatus sees the matching enum, not the previous attempt.
	Self->LastLoginStatus.store(Status, std::memory_order_release);
	if (Status == ATLAS_LOGIN_SUCCESS)
	{
		Self->State.store(EAtlasNetClientState::LoginSucceeded);
	}
	else
	{
		UE_LOG(LogAtlasNet, Warning, TEXT("Login failed status=%u: %s"), Status,
			ErrorMessage ? ANSI_TO_TCHAR(ErrorMessage) : TEXT("(no message)"));
		Self->State.store(EAtlasNetClientState::LoginFailed);
	}
}

void FAtlasNetClient::OnAuthResultStatic(void* UserData, uint8 Success, uint32 EntityId,
                                         uint16 TypeId, const char* ErrorMessage)
{
	if (UserData == nullptr) return;
	FAtlasNetClient* Self = static_cast<FAtlasNetClient*>(UserData);
	if (Success != 0)
	{
		Self->PlayerEntityId.store(EntityId);
		Self->PlayerTypeId.store(TypeId);
		Self->State.store(EAtlasNetClientState::Authenticated);
	}
	else
	{
		UE_LOG(LogAtlasNet, Warning, TEXT("Auth failed: %s"),
			ErrorMessage ? ANSI_TO_TCHAR(ErrorMessage) : TEXT("(no message)"));
		Self->State.store(EAtlasNetClientState::AuthFailed);
	}
}

void FAtlasNetClient::OnDeliverStatic(AtlasNetContext* Ctx, uint16 MsgId, const uint8* Payload,
                                      int32 Len)
{
	FAtlasNetClient* Self = FindByCtx(Ctx);
	if (!Self) return;
	if (Len > kAtlasMaxInboundPayloadBytes)
	{
		UE_LOG(LogAtlasNet, Warning,
			TEXT("Dropping oversize inbound payload msg_id=0x%04X len=%d (cap=%d)"),
			MsgId, Len, kAtlasMaxInboundPayloadBytes);
		return;
	}
	const int32 Depth = Self->InboundDepth.load(std::memory_order_relaxed);
	if (Depth >= kAtlasInboundQueueCap)
	{
		// Latch the first overflow report so a frozen game thread doesn't generate
		// a log line per net-thread poll.
		bool Expected = false;
		if (Self->bInboundOverflowLogged.compare_exchange_strong(Expected, true))
		{
			UE_LOG(LogAtlasNet, Warning,
				TEXT("Inbound queue overflow (cap=%d) — dropping msg_id=0x%04X; "
				     "game thread likely stalled"), kAtlasInboundQueueCap, MsgId);
		}
		return;
	}
	if (Depth == kAtlasInboundQueueWarnThreshold)
	{
		UE_LOG(LogAtlasNet, Warning,
			TEXT("Inbound queue depth reached %d — game thread falling behind"), Depth);
	}
	FAtlasInboundMessage Msg;
	Msg.MsgId = MsgId;
	if (Len > 0 && Payload != nullptr)
	{
		Msg.Payload.Append(Payload, Len);
	}
	Self->Inbound.Enqueue(MoveTemp(Msg));
	Self->InboundDepth.fetch_add(1, std::memory_order_relaxed);
}

void FAtlasNetClient::OnDisconnectStatic(AtlasNetContext* Ctx, int32 Reason)
{
	FAtlasNetClient* Self = FindByCtx(Ctx);
	if (!Self) return;
	// Funnel through the inbound queue so the game thread sees disconnect AFTER
	// every message already in flight, not interleaved with them. Bypasses the
	// depth cap on purpose — losing the disconnect would leave the game in a
	// permanent "still connected" state.
	FAtlasInboundMessage Msg;
	Msg.MsgId = kAtlasInboundDisconnectSentinel;
	Msg.Payload.SetNumUninitialized(sizeof(int32));
	FMemory::Memcpy(Msg.Payload.GetData(), &Reason, sizeof(int32));
	Self->Inbound.Enqueue(MoveTemp(Msg));
	Self->InboundDepth.fetch_add(1, std::memory_order_relaxed);
}

void FAtlasNetClient::DeliverForTest(uint16 MsgId, const uint8* Payload, int32 Len)
{
	OnDeliverStatic(Ctx, MsgId, Payload, Len);
}

void FAtlasNetClient::TriggerDisconnectForTest(int32 Reason)
{
	OnDisconnectStatic(Ctx, Reason);
}
