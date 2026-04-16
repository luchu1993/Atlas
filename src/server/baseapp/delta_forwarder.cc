#include "delta_forwarder.h"

#include <algorithm>

#include "network/channel.h"

namespace atlas {

void DeltaForwarder::Enqueue(EntityID entity_id, std::span<const std::byte> delta) {
  // Replace existing entry for the same entity (only latest matters).
  for (auto& entry : queue_) {
    if (entry.entity_id == entity_id) {
      entry.delta.assign(delta.begin(), delta.end());
      // Keep accumulated deferred_ticks — the entity has been waiting.
      return;
    }
  }

  queue_.push_back(PendingDelta{entity_id, {delta.begin(), delta.end()}, 0});
}

auto DeltaForwarder::Flush(Channel& client_ch, uint32_t budget_bytes) -> uint32_t {
  if (queue_.empty()) return 0;

  // Sort by deferred_ticks descending so starved entries go first.
  std::sort(queue_.begin(), queue_.end(), [](const PendingDelta& a, const PendingDelta& b) {
    return a.deferred_ticks > b.deferred_ticks;
  });

  uint32_t bytes_sent = 0;
  std::size_t sent_count = 0;

  for (auto& entry : queue_) {
    auto entry_size = static_cast<uint32_t>(entry.delta.size());
    if (bytes_sent + entry_size > budget_bytes && sent_count > 0) {
      // Would exceed budget and we already sent at least one — stop.
      break;
    }

    // Always send at least one delta per flush to guarantee progress.
    (void)client_ch.SendMessage(kClientDeltaMessageId, std::span<const std::byte>(entry.delta));
    bytes_sent += entry_size;
    ++sent_count;
  }

  stats_.bytes_sent += bytes_sent;

  // Remove sent entries, bump deferred_ticks on survivors.
  uint64_t deferred_bytes = 0;
  if (sent_count < queue_.size()) {
    for (std::size_t i = sent_count; i < queue_.size(); ++i) {
      ++queue_[i].deferred_ticks;
      deferred_bytes += queue_[i].delta.size();
    }
  }
  stats_.bytes_deferred += deferred_bytes;

  queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(sent_count));
  return bytes_sent;
}

}  // namespace atlas
