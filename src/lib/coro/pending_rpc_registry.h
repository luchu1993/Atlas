#ifndef ATLAS_LIB_CORO_PENDING_RPC_REGISTRY_H_
#define ATLAS_LIB_CORO_PENDING_RPC_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <unordered_map>

#include "foundation/clock.h"
#include "foundation/error.h"
#include "foundation/timer_queue.h"
#include "network/event_dispatcher.h"
#include "network/message.h"

namespace atlas {

class PendingRpcRegistry {
 public:
  explicit PendingRpcRegistry(EventDispatcher& dispatcher);
  ~PendingRpcRegistry();

  PendingRpcRegistry(const PendingRpcRegistry&) = delete;
  PendingRpcRegistry& operator=(const PendingRpcRegistry&) = delete;

  using ReplyCallback = std::function<void(std::span<const std::byte> payload)>;
  using ErrorCallback = std::function<void(Error error)>;

  struct PendingHandle {
    MessageID reply_id{0};
    uint32_t request_id{0};
    [[nodiscard]] auto IsValid() const -> bool { return reply_id != 0; }
  };

  auto RegisterPending(MessageID reply_id, uint32_t request_id, ReplyCallback on_reply,
                       ErrorCallback on_error, Duration timeout) -> PendingHandle;

  // request_id is the first little-endian uint32_t in payload.
  auto TryDispatch(MessageID id, std::span<const std::byte> payload) -> bool;

  void Cancel(PendingHandle handle);

  void CancelAll();

  [[nodiscard]] auto PendingCount() const -> size_t;

 private:
  struct PendingKey {
    MessageID reply_id;
    uint32_t request_id;
    auto operator==(const PendingKey&) const -> bool = default;
  };

  struct PendingKeyHash {
    auto operator()(const PendingKey& k) const -> size_t {
      auto h = static_cast<size_t>(k.reply_id);
      h ^= std::hash<uint32_t>{}(k.request_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct PendingEntry {
    ReplyCallback on_reply;
    ErrorCallback on_error;
    TimerHandle timeout_timer;
  };

  static auto ExtractRequestId(std::span<const std::byte> payload) -> uint32_t;

  std::unordered_map<PendingKey, PendingEntry, PendingKeyHash> pending_;
  EventDispatcher& dispatcher_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_PENDING_RPC_REGISTRY_H_
