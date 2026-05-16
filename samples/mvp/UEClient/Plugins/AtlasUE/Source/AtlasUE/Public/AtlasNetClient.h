#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"

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

// Owns one AtlasNetContext. Login + auth pump on the GameThread; after auth a
// worker thread takes over polling and delivers messages via the SPSC queue.
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
	// on_deliver / on_disconnect have ctx only — recover `this` via the static
	// registry; on_login / on_auth carry user_data so the cast is direct.
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
