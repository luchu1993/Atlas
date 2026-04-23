#include "delta_forwarder.h"

#include <algorithm>

#include "network/channel.h"

namespace atlas {

void DeltaForwarder::Enqueue(EntityID entity_id, std::span<const std::byte> delta,
                             uint16_t priority) {
  // Replace existing entry for the same entity (only latest matters).
  for (auto& entry : queue_) {
    if (entry.entity_id == entity_id) {
      entry.delta.assign(delta.begin(), delta.end());
      // Keep accumulated deferred_ticks — the entity has been waiting.
      // Priority is max-merged so a low-priority write can't demote an
      // entry an earlier high-priority producer deliberately boosted.
      entry.priority = std::max(entry.priority, priority);
      return;
    }
  }

  queue_.push_back(PendingDelta{entity_id, {delta.begin(), delta.end()}, 0, priority});
}

auto DeltaForwarder::Flush(Channel& client_ch, uint32_t budget_bytes) -> uint32_t {
  if (queue_.empty()) return 0;

  uint32_t bytes_sent = 0;

  // Pass 1: force-send starved entries regardless of budget or priority.
  // A steady stream of higher-priority traffic can otherwise keep a
  // low-priority entry indefinitely unsent; the deferred_ticks cap
  // guarantees progress once it's been waiting long enough.
  auto starved_begin = std::partition(queue_.begin(), queue_.end(), [](const PendingDelta& e) {
    return e.deferred_ticks < kMaxDeferredTicks;
  });
  for (auto it = starved_begin; it != queue_.end(); ++it) {
    (void)client_ch.SendMessage(kClientDeltaMessageId, std::span<const std::byte>(it->delta));
    bytes_sent += static_cast<uint32_t>(it->delta.size());
    ++stats_.force_sent_count;
  }
  queue_.erase(starved_begin, queue_.end());

  // Pass 2: sort remaining descending by (priority, deferred_ticks) and
  // send while the byte budget holds. Budget is measured from scratch
  // this tick — force-sends in Pass 1 don't eat into it, since they
  // already proved starvation dominates the budget knob.
  std::sort(queue_.begin(), queue_.end(), [](const PendingDelta& a, const PendingDelta& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.deferred_ticks > b.deferred_ticks;
  });

  uint32_t pass2_bytes = 0;
  std::size_t sent_count = 0;
  for (auto& entry : queue_) {
    auto entry_size = static_cast<uint32_t>(entry.delta.size());
    if (pass2_bytes + entry_size > budget_bytes && sent_count > 0) {
      break;
    }
    (void)client_ch.SendMessage(kClientDeltaMessageId, std::span<const std::byte>(entry.delta));
    pass2_bytes += entry_size;
    ++sent_count;
  }
  bytes_sent += pass2_bytes;
  stats_.bytes_sent += bytes_sent;

  // Remove sent entries, bump deferred_ticks on survivors.
  uint64_t deferred_bytes = 0;
  for (std::size_t i = sent_count; i < queue_.size(); ++i) {
    ++queue_[i].deferred_ticks;
    deferred_bytes += queue_[i].delta.size();
  }
  stats_.bytes_deferred += deferred_bytes;

  queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(sent_count));
  return bytes_sent;
}

}  // namespace atlas
