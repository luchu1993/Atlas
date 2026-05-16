#include "AtlasNetClient.h"

#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "Logging/LogMacros.h"

#include "AtlasCore/aoi_envelope.h"
#include "AtlasCore/client_entity_manager.h"

DEFINE_LOG_CATEGORY_STATIC(LogAtlasNet, Log, All);

FCriticalSection FAtlasNetClient::ContextRegistryMutex;
TMap<AtlasNetContext*, FAtlasNetClient*> FAtlasNetClient::ContextRegistry;

uint32 FAtlasNetRunnable::Run()
{
	while (!bStop.load(std::memory_order_acquire))
	{
		AtlasNetPoll(Ctx);
		FPlatformProcess::Sleep(0.005f);
	}
	return 0;
}

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

	FAtlasInboundMessage Drain;
	while (Inbound.Dequeue(Drain)) {}
	PendingDisconnect.store(-1);
	PlayerEntityId.store(0);
	PlayerTypeId.store(0);
	State.store(EAtlasNetClientState::Idle);
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
		if (Msg.MsgId == 0xF001 || Msg.MsgId == 0xF003)
		{
			atlas::DecodeAoIEnvelope(Msg.Payload.GetData(), Msg.Payload.Num(), Manager);
		}
		else if (Msg.MsgId == 2024 && Msg.Payload.Num() >= 6)
		{
			// kEntityTransferred (baseapp_messages.h): [u32 entity_id][u16 type_id].
			uint32 NewEntityId = 0;
			uint16 NewTypeId = 0;
			FMemory::Memcpy(&NewEntityId, Msg.Payload.GetData(), 4);
			FMemory::Memcpy(&NewTypeId, Msg.Payload.GetData() + 4, 2);
			Manager.HandleCreate(NewEntityId, NewTypeId);
		}
		// 0xF002 baseline, 0xF004 RPC, 2025 cell-ready handled by later milestones.
	}

	const int32 Reason = PendingDisconnect.exchange(-1);
	if (Reason != -1)
	{
		UE_LOG(LogAtlasNet, Log, TEXT("AtlasNet disconnect reason=%d"), Reason);
		State.store(EAtlasNetClientState::Disconnected);
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
	FAtlasInboundMessage Msg;
	Msg.MsgId = MsgId;
	if (Len > 0 && Payload != nullptr)
	{
		Msg.Payload.Append(Payload, Len);
	}
	Self->Inbound.Enqueue(MoveTemp(Msg));
}

void FAtlasNetClient::OnDisconnectStatic(AtlasNetContext* Ctx, int32 Reason)
{
	FAtlasNetClient* Self = FindByCtx(Ctx);
	if (!Self) return;
	Self->PendingDisconnect.store(Reason);
}
