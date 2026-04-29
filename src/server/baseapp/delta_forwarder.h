#ifndef ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_
#define ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "server/entity_types.h"

namespace atlas {

class Channel;

// Per-client UNRELIABLE latest-wins delta relay for ReplicatedDeltaFromCell
// (msg 2015) → client msg 0xF001. Same-entity writes replace the queued
// entry. INVARIANT: only volatile state (pos/orientation); cumulative state
// must use ReplicatedReliableDeltaFromCell (msg 2017) or it will be dropped.
class DeltaForwarder {
 public:
  struct Stats {
    uint64_t bytes_sent{0};
    uint64_t bytes_deferred{0};
    uint64_t force_sent_count{0};  // entries flushed past the budget cap
  };

  // Force-send threshold: prevents starvation behind steady high-priority
  // traffic.
  static constexpr uint32_t kMaxDeferredTicks = 120;

  // Replace takes max(existing, new) priority so a low-priority write
  // cannot demote an entry an earlier producer deliberately boosted.
  void Enqueue(EntityID entity_id, std::span<const std::byte> delta, uint16_t priority = 0);

  // Returns bytes actually sent.
  auto Flush(Channel& client_ch, uint32_t budget_bytes) -> uint32_t;

  [[nodiscard]] auto QueueDepth() const -> std::size_t { return queue_.size(); }

  [[nodiscard]] auto GetStats() const -> const Stats& { return stats_; }

 private:
  struct PendingDelta {
    EntityID entity_id{kInvalidEntityID};
    std::vector<std::byte> delta;
    uint32_t deferred_ticks{0};
    uint16_t priority{0};
  };

  std::vector<PendingDelta> queue_;
  Stats stats_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_
