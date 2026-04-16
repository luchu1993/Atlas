#include "coro/pending_rpc_registry.h"

#include "foundation/log.h"
#include "serialization/binary_stream.h"

namespace atlas {

PendingRpcRegistry::PendingRpcRegistry(EventDispatcher& dispatcher) : dispatcher_(dispatcher) {}

PendingRpcRegistry::~PendingRpcRegistry() {
  CancelAll();
}

auto PendingRpcRegistry::RegisterPending(MessageID reply_id, uint32_t request_id,
                                         ReplyCallback on_reply, ErrorCallback on_error,
                                         Duration timeout) -> PendingHandle {
  auto key = PendingKey{reply_id, request_id};

  // Cancel existing entry if present (prevents timer leak on duplicate keys)
  if (auto it = pending_.find(key); it != pending_.end()) {
    ATLAS_LOG_WARNING("PendingRpcRegistry: overwriting duplicate key (reply={}, req={})", reply_id,
                      request_id);
    dispatcher_.CancelTimer(it->second.timeout_timer);
    pending_.erase(it);
  }

  // Start timeout timer
  auto timer = dispatcher_.AddTimer(timeout, [this, key](TimerHandle) {
    auto it = pending_.find(key);
    if (it == pending_.end()) return;

    auto error_cb = std::move(it->second.on_error);
    pending_.erase(it);

    if (error_cb) error_cb(Error{ErrorCode::kTimeout, "RPC timed out"});
  });

  pending_[key] = PendingEntry{std::move(on_reply), std::move(on_error), timer};
  return PendingHandle{reply_id, request_id};
}

auto PendingRpcRegistry::TryDispatch(MessageID id, std::span<const std::byte> payload) -> bool {
  if (payload.size() < sizeof(uint32_t)) return false;

  auto request_id = ExtractRequestId(payload);
  auto key = PendingKey{id, request_id};
  auto it = pending_.find(key);

  if (it == pending_.end()) return false;

  // Cancel timeout timer
  dispatcher_.CancelTimer(it->second.timeout_timer);

  // Move callback out before erasing (prevent reentry issues)
  auto on_reply = std::move(it->second.on_reply);
  pending_.erase(it);

  on_reply(payload);
  return true;
}

void PendingRpcRegistry::Cancel(PendingHandle handle) {
  auto key = PendingKey{handle.reply_id, handle.request_id};
  auto it = pending_.find(key);
  if (it == pending_.end()) return;

  dispatcher_.CancelTimer(it->second.timeout_timer);

  auto error_cb = std::move(it->second.on_error);
  pending_.erase(it);

  if (error_cb) error_cb(Error{ErrorCode::kCancelled, "RPC cancelled"});
}

void PendingRpcRegistry::CancelAll() {
  // Copy keys to avoid iterator invalidation (callbacks may re-enter)
  auto entries = std::move(pending_);
  pending_.clear();

  for (auto& [key, entry] : entries) {
    dispatcher_.CancelTimer(entry.timeout_timer);
    if (entry.on_error) entry.on_error(Error{ErrorCode::kCancelled, "RPC registry shutting down"});
  }
}

auto PendingRpcRegistry::PendingCount() const -> size_t {
  return pending_.size();
}

auto PendingRpcRegistry::ExtractRequestId(std::span<const std::byte> payload) -> uint32_t {
  uint32_t id_le = 0;
  std::memcpy(&id_le, payload.data(), sizeof(uint32_t));
  return endian::FromLittle(id_le);
}

}  // namespace atlas
