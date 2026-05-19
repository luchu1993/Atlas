#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"

#include <atomic>

#include "net_client/client_api.h"

#include "AtlasNetClient.generated.h"

namespace atlas { class ClientEntityManager; }

UENUM(BlueprintType)
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

// Disconnect notification rides the inbound SPSC queue so the game thread
// observes it after every in-flight message; 0xFFFE is above every server msg_id.
inline constexpr uint16 kAtlasInboundDisconnectSentinel = 0xFFFE;

// Drop new inbound messages past this depth so a stalled game thread can't
// grow the queue without bound; warn once when crossing the threshold.
inline constexpr int32 kAtlasInboundQueueCap = 8192;
inline constexpr int32 kAtlasInboundQueueWarnThreshold = 1024;
// Hard cap on a single inbound payload; legitimate baselines / RPCs stay
// well under this (server RPC cap is 64 KiB).
inline constexpr int32 kAtlasMaxInboundPayloadBytes = 1 * 1024 * 1024;

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

	// Test-only hooks driving the same paths as the SDK callbacks without a real server.
	void DeliverForTest(uint16 MsgId, const uint8* Payload, int32 Len);
	void TriggerDisconnectForTest(int32 Reason);
	[[nodiscard]] int32 GetInboundDepthForTest() const { return InboundDepth.load(std::memory_order_relaxed); }
	[[nodiscard]] bool GetInboundOverflowLoggedForTest() const { return bInboundOverflowLogged.load(std::memory_order_relaxed); }

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
	// Side counter so the producer can shed load when the game thread stalls.
	// Best-effort: relaxed RMW can race past the cap by one or two messages.
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
