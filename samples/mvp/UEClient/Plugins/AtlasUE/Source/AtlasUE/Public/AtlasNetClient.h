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

// Synthetic msg_id used to put disconnect notifications through the same
// SPSC queue as AoI / RPC traffic; otherwise an out-of-band PendingDisconnect
// atomic could fire while the game thread is still draining earlier messages,
// causing one frame of post-disconnect stale dispatch. The value lives in the
// top of the msg_id range that no server-side category claims (kReserved /
// kLogin / kBaseApp etc. all stay well under 0x8000), so it can't collide
// with a real OnDeliver dispatch.
inline constexpr uint16 kAtlasInboundDisconnectSentinel = 0xFFFE;

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

	[[nodiscard]] EAtlasNetClientState GetState() const { return State.load(std::memory_order_acquire); }
	[[nodiscard]] uint32 GetPlayerEntityId() const { return PlayerEntityId.load(); }
	[[nodiscard]] uint16 GetPlayerTypeId() const { return PlayerTypeId.load(); }
	// AtlasLoginStatus from the most recent LoginResult; 0xFF when no result yet.
	[[nodiscard]] uint8 GetLastLoginStatus() const { return LastLoginStatus.load(std::memory_order_acquire); }
	[[nodiscard]] AtlasNetContext* GetContext() const { return Ctx; }

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
	// SPSC TQueue has no size() — track depth via a side counter so the
	// producer (net thread) can shed load before the queue grows unbounded
	// when the game thread stalls (e.g. paused in the debugger). Best-effort
	// only: relaxed ordering means the cap check can race the producer's own
	// fetch_add and let one or two extra messages through, which is fine —
	// the goal is stall-shedding, not a hard bound.
	std::atomic<int32> InboundDepth{0};
	std::atomic<bool> bInboundOverflowLogged{false};

	std::atomic<EAtlasNetClientState> State{EAtlasNetClientState::Idle};
	std::atomic<uint32> PlayerEntityId{0};
	std::atomic<uint16> PlayerTypeId{0};
	// 0xFF = no LoginResult observed yet; otherwise AtlasLoginStatus enum.
	std::atomic<uint8> LastLoginStatus{0xFF};

	FAtlasNetRunnable* Runnable = nullptr;
	FRunnableThread* Thread = nullptr;
};
