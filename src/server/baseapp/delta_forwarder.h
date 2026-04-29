#ifndef ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_
#define ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "server/entity_types.h"

namespace atlas {

class Channel;

// ============================================================================
// DeltaForwarder — per-client UNRELIABLE latest-wins delta relay.
//
// Queues ReplicatedDeltaFromCell (msg 2015) payloads and flushes them each
// tick under a byte budget. Same-entity writes replace the queued entry
// (latest-wins); deferred_ticks boosts starved entries on the next flush.
//
// INVARIANT: only unreliable volatile state (position/orientation) goes
// here — anything cumulative must use ReplicatedReliableDeltaFromCell
// (msg 2017, path #2 below) or its frames will be silently dropped.
//
// CellApp → Client paths:
//   1. ReplicatedDeltaFromCell         (msg 2015, Unreliable)
//        → this forwarder → client msg 0xF001
//   2. ReplicatedReliableDeltaFromCell (msg 2017, Reliable)
//        → direct to client channel   → client msg 0xF003
//   3. SelfRpcFromCell / BroadcastRpcFromCell (msg 2014 / 2016)
//        → BaseApp::RelayRpcToClient   → client msg 0xF004 (envelope:
//                                        u32 rpc_id + serialized args)
// ============================================================================

class DeltaForwarder {
 public:
  struct Stats {
    uint64_t bytes_sent{0};
    uint64_t bytes_deferred{0};
    uint64_t force_sent_count{0};  // entries flushed past the budget cap
  };

  // Starvation cap: an entry that has been deferred this many consecutive
  // flushes is force-sent on the next Flush, regardless of the byte
  // budget. Prevents an entity stuck behind a steady stream of
  // higher-priority traffic from waiting forever.
  static constexpr uint32_t kMaxDeferredTicks = 120;

  /// Enqueue or replace a delta for the given entity.
  /// `priority` biases Flush ordering — higher goes first. A same-entity
  /// replace takes max(existing_priority, new_priority) so a low-priority
  /// write can't demote an entry an earlier high-priority producer
  /// deliberately boosted.
  void Enqueue(EntityID entity_id, std::span<const std::byte> delta, uint16_t priority = 0);

  /// Flush queued deltas to `client_ch` using reserved message ID,
  /// stopping when `budget_bytes` would be exceeded.
  /// Returns the number of bytes actually sent.
  auto Flush(Channel& client_ch, uint32_t budget_bytes) -> uint32_t;

  /// Current queue depth (number of pending entities).
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
