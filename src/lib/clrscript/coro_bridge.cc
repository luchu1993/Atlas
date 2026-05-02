#include "clrscript/coro_bridge.h"

#include <chrono>

#include "foundation/error.h"

namespace atlas::coro_bridge {

namespace {

constexpr int32_t kStatusSuccess = 0;
constexpr int32_t kStatusTimeout = 1;
constexpr int32_t kStatusCancelled = 2;
constexpr int32_t kStatusSendError = 3;

// "Never times out" is approximated by a year — the dispatcher won't reach it.
constexpr auto kNoTimeoutDuration = std::chrono::hours{24 * 365};

auto PackHandle(uint16_t reply_id, uint32_t request_id) -> uint64_t {
  return (static_cast<uint64_t>(reply_id) << 32) | static_cast<uint64_t>(request_id);
}

auto UnpackHandle(uint64_t handle) -> PendingRpcRegistry::PendingHandle {
  return PendingRpcRegistry::PendingHandle{
      static_cast<MessageID>(handle >> 32),
      static_cast<uint32_t>(handle & 0xFFFFFFFFu),
  };
}

}  // namespace

auto RegisterPending(PendingRpcRegistry& registry, CoroOnRpcCompleteFn on_complete,
                     uint16_t reply_id, uint32_t request_id, int32_t timeout_ms,
                     intptr_t managed_handle) -> uint64_t {
  if (on_complete == nullptr) return 0;

  auto timeout = timeout_ms > 0 ? std::chrono::milliseconds{timeout_ms}
                                : std::chrono::duration_cast<Duration>(kNoTimeoutDuration);

  registry.RegisterPending(
      reply_id, request_id,
      [on_complete, managed_handle](std::span<const std::byte> payload) {
        on_complete(managed_handle, kStatusSuccess,
                    reinterpret_cast<const uint8_t*>(payload.data()),
                    static_cast<int32_t>(payload.size()));
      },
      [on_complete, managed_handle](Error err) {
        int32_t status = kStatusSendError;
        if (err.Code() == ErrorCode::kTimeout) status = kStatusTimeout;
        else if (err.Code() == ErrorCode::kCancelled) status = kStatusCancelled;
        on_complete(managed_handle, status, nullptr, 0);
      },
      timeout);

  return PackHandle(reply_id, request_id);
}

void CancelPending(PendingRpcRegistry& registry, uint64_t handle) {
  if (handle == 0) return;
  registry.Cancel(UnpackHandle(handle));
}

}  // namespace atlas::coro_bridge
