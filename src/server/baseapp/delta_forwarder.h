#ifndef ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_
#define ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "network/message.h"
#include "server/entity_types.h"

namespace atlas {

class Channel;

// ============================================================================
// DeltaForwarder — per-client UNRELIABLE latest-wins delta relay
//
// CellApp sends property deltas (ReplicatedDeltaFromCell, msg 2015) to
// BaseApp, which forwards them to each client.  When many entities change in
// one tick the aggregate byte volume can spike.  DeltaForwarder queues
// incoming deltas and flushes them to the client channel each tick,
// respecting a byte budget.
//
// Deltas that exceed the budget stay queued; their deferred_ticks counter
// increments each tick, boosting their priority so starvation cannot occur.
// When the same entity receives a new delta while a previous one is still
// queued, the old entry is replaced — the client only needs the latest state.
//
// -------- CellApp → Client delta path contract (three paths) -----------------
//
// The "queued-then-flushed with same-entity replacement" policy above is
// intentionally LATEST-WINS.  That is correct for the unreliable volatile
// path (position/orientation updates where stale frames are worthless) but
// WRONG for anything that requires ordered, cumulative delivery.  Route
// choice is made at the CellApp send site by selecting the message type:
//
//   1. ReplicatedDeltaFromCell        (msg 2015, Unreliable)
//        → BaseApp::OnReplicatedDeltaFromCell()
//        → THIS forwarder (latest-wins, byte-budgeted)
//        → client via kClientDeltaMessageId (0xF001)
//      Semantics: latest-wins / best-effort.
//      Use for: Volatile position/orientation updates only.
//
//   2. ReplicatedReliableDeltaFromCell (msg 2017, Reliable)
//        → BaseApp::OnReplicatedReliableDeltaFromCell()
//        → DIRECTLY to client channel — bypasses this forwarder
//        → client via kClientReliableDeltaMessageId (0xF003)
//      Semantics: ordered, every delta delivered (transport retransmits).
//      Use for: property deltas where cumulative state matters (HP, state,
//      inventory, AoI property updates carrying event_seq).
//
//   3. SelfRpcFromCell / BroadcastRpcFromCell (msg 2014 / 2016)
//        → BaseApp::OnSelfRpcFromCell / OnBroadcastRpcFromCell()
//        → DIRECTLY to client channel — bypasses this forwarder
//        → client via the msg's rpc_id (dynamic, not a reserved id)
//      Use for: RPC method invocations on the owner's client entity.
//
// INVARIANT: Property deltas carrying event_seq or any cumulative counter
// MUST use path #2 (ReplicatedReliableDeltaFromCell).  Enqueueing them here
// would drop intermediate frames (same-entity replacement) and silently
// desynchronize the client.  The architectural separation is enforced at
// CellApp's send site — there is no runtime DCHECK here because the payload
// format is opaque at this layer.
// ============================================================================

class DeltaForwarder {
 public:
  struct Stats {
    uint64_t bytes_sent{0};
    uint64_t bytes_deferred{0};
  };

  /// Enqueue or replace a delta for the given entity.
  /// `priority` biases Flush ordering — higher goes first. A same-entity
  /// replace takes max(existing_priority, new_priority) so a low-priority
  /// write can't demote an entry an earlier high-priority producer
  /// deliberately boosted. Default 0 — BaseApp today has no priority
  /// context (Witness-side distance/priority scoring is Phase 10 work);
  /// the field is wired now so the call site is stable once a real
  /// value lands.
  void Enqueue(EntityID entity_id, std::span<const std::byte> delta, uint16_t priority = 0);

  /// Flush queued deltas to `client_ch` using reserved message ID,
  /// stopping when `budget_bytes` would be exceeded.
  /// Returns the number of bytes actually sent.
  auto Flush(Channel& client_ch, uint32_t budget_bytes) -> uint32_t;

  /// Current queue depth (number of pending entities).
  [[nodiscard]] auto QueueDepth() const -> std::size_t { return queue_.size(); }

  [[nodiscard]] auto GetStats() const -> const Stats& { return stats_; }

  /// Reserved client-facing message ID for (unreliable) delta updates.
  static constexpr MessageID kClientDeltaMessageId = static_cast<MessageID>(0xF001);

  /// Reserved client-facing message ID for periodic full-state baseline snapshots —
  /// sent reliably every `kBaselineInterval` ticks so a UDP loss window cannot
  /// leave the client stuck on stale state. The payload is the owner-scope
  /// serialization produced by the source generator.
  static constexpr MessageID kClientBaselineMessageId = static_cast<MessageID>(0xF002);

  /// Reserved client-facing message ID for reliable property delta updates —
  /// carries fields marked reliable="true" in .def; bypasses the byte budget.
  static constexpr MessageID kClientReliableDeltaMessageId = static_cast<MessageID>(0xF003);

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
