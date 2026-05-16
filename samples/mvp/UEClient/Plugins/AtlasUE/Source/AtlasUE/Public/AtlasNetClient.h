#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"

#include <atomic>

#include "net_client/client_api.h"

namespace atlas { class ClientEntityManager; }

enum class EAtlasNetClientState : uint8
{
	Idle,
	LoggingIn,
	LoginFailed,
	LoginSucceeded,
	Authenticating,
	AuthFailed,
	Authenticated,
	Running,
	Disconnected,
};

struct FAtlasInboundMessage
{
	uint16 MsgId = 0;
	TArray<uint8> Payload;
};

class FAtlasNetRunnable;

// Owns one AtlasNetContext. Login + authentication are GameThread-driven via
// PollOnGameThread; after a successful auth the caller invokes StartRunningThread
// and ongoing AtlasNetPoll moves to a dedicated worker, delivering inbound
// messages via an SPSC queue that the GameThread drains in TickGameThread.
class FAtlasNetClient
{
public:
	FAtlasNetClient();
	~FAtlasNetClient();

	FAtlasNetClient(const FAtlasNetClient&) = delete;
	FAtlasNetClient& operator=(const FAtlasNetClient&) = delete;

	bool Create();
	void Destroy();

	bool BeginLogin(const FString& Host, uint16 Port, const FString& Username,
	                const FString& PasswordHash);
	bool BeginAuthenticate();

	void PollOnGameThread();
	void StartRunningThread();

	void TickGameThread(atlas::ClientEntityManager& Manager);

	EAtlasNetClientState GetState() const { return State.load(std::memory_order_acquire); }
	uint32 GetPlayerEntityId() const { return PlayerEntityId.load(); }
	uint16 GetPlayerTypeId() const { return PlayerTypeId.load(); }
	AtlasNetContext* GetContext() const { return Ctx; }

private:
	// C ABI shims. on_deliver / on_disconnect have ctx only — look up `this` via
	// a static registry; on_login / on_auth carry user_data so we pass `this`
	// directly and skip the registry.
	static void OnLoginResultStatic(void* UserData, uint8 Status, const char* Host,
	                                uint16 Port, const char* ErrorMessage);
	static void OnAuthResultStatic(void* UserData, uint8 Success, uint32 EntityId,
	                               uint16 TypeId, const char* ErrorMessage);
	static void OnDeliverStatic(AtlasNetContext* Ctx, uint16 MsgId, const uint8* Payload,
	                            int32 Len);
	static void OnDisconnectStatic(AtlasNetContext* Ctx, int32 Reason);

	static FAtlasNetClient* FindByCtx(AtlasNetContext* Ctx);

	static FCriticalSection ContextRegistryMutex;
	static TMap<AtlasNetContext*, FAtlasNetClient*> ContextRegistry;

	AtlasNetContext* Ctx = nullptr;

	TQueue<FAtlasInboundMessage, EQueueMode::Spsc> Inbound;
	std::atomic<int32> PendingDisconnect{-1};

	std::atomic<EAtlasNetClientState> State{EAtlasNetClientState::Idle};
	std::atomic<uint32> PlayerEntityId{0};
	std::atomic<uint16> PlayerTypeId{0};

	FAtlasNetRunnable* Runnable = nullptr;
	FRunnableThread* Thread = nullptr;
};

class FAtlasNetRunnable : public FRunnable
{
public:
	explicit FAtlasNetRunnable(AtlasNetContext* InCtx) : Ctx(InCtx) {}

	bool Init() override { return true; }
	uint32 Run() override;
	void Stop() override { bStop.store(true, std::memory_order_release); }
	void Exit() override {}

private:
	AtlasNetContext* Ctx = nullptr;
	std::atomic<bool> bStop{false};
};
