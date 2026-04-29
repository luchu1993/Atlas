#ifndef ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_
#define ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "foundation/error.h"
#include "network/message.h"
#include "serialization/binary_stream.h"
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

  /// Reserved client-facing message ID for ALL server → client RPCs
  /// (entity-level and component-level alike).  Wire body:
  ///   [u32 rpc_id (slot:8 | method:24)] [serialized args ...]
  /// The client default handler unwraps the rpc_id and dispatches via
  /// the C# DispatchRpc callback.  One envelope keeps the protocol-
  /// level MessageID space (u16) distinct from the application-level
  /// rpc_id space (u32) — see BaseApp::RelayRpcToClient.
  static constexpr MessageID kClientRpcMessageId = static_cast<MessageID>(0xF004);

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

// ============================================================================
// Send-only envelopes for the three reserved client-facing wire ids.
//
// All three carry opaque body bytes; the C++ client intercepts them via its
// SetDefaultHandler dispatch (state-channel forward / RPC unwrap), so no
// Deserialize is ever invoked.  Span variants — caller owns the storage
// for the duration of the synchronous SendMessage call.
//
// Defining these as NetworkMessage descriptors lets each one carry its
// own (reliability × urgency) pair, fixing two semantic bugs that the
// raw `SendMessage(MessageID, span)` fallback used to mask:
//   • Unreliable lane (0xF001) was going via reliable Send because the
//     fallback descriptor defaulted to kReliable.
//   • RPCs (0xF004) were silently kBatched because the fallback default
//     flipped to kBatched in 0043, contradicting the audit's
//     PvP-latency-critical kImmediate verdict.
// ============================================================================

// 0xF001 — unreliable volatile delta (latest-wins per entity).
struct ClientDeltaEnvelope {
  std::span<const std::byte> bytes;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        DeltaForwarder::kClientDeltaMessageId, "baseapp::ClientDeltaEnvelope",
        MessageLengthStyle::kVariable,         -1,
        MessageReliability::kUnreliable,       MessageUrgency::kBatched};
    return kDesc;
  }
  void Serialize(BinaryWriter& w) const { w.WriteBytes(bytes); }
  static auto Deserialize(BinaryReader&) -> Result<ClientDeltaEnvelope> {
    return Error{ErrorCode::kInvalidArgument, "ClientDeltaEnvelope is send-only"};
  }
};
static_assert(NetworkMessage<ClientDeltaEnvelope>);

// 0xF003 — reliable property delta (cumulative state, must not drop).
struct ClientReliableDeltaEnvelope {
  std::span<const std::byte> bytes;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{DeltaForwarder::kClientReliableDeltaMessageId,
                                   "baseapp::ClientReliableDeltaEnvelope",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }
  void Serialize(BinaryWriter& w) const { w.WriteBytes(bytes); }
  static auto Deserialize(BinaryReader&) -> Result<ClientReliableDeltaEnvelope> {
    return Error{ErrorCode::kInvalidArgument, "ClientReliableDeltaEnvelope is send-only"};
  }
};
static_assert(NetworkMessage<ClientReliableDeltaEnvelope>);

// 0xF004 — unified server → client RPC envelope.  Body layout:
//   [u32 rpc_id (slot:8 | method:24)] [args bytes ...]
struct ClientRpcEnvelope {
  uint32_t rpc_id{0};
  std::span<const std::byte> args;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        DeltaForwarder::kClientRpcMessageId, "baseapp::ClientRpcEnvelope",
        MessageLengthStyle::kVariable,       -1,
        MessageReliability::kReliable,       MessageUrgency::kImmediate};
    return kDesc;
  }
  void Serialize(BinaryWriter& w) const {
    w.Write(rpc_id);
    w.WriteBytes(args);
  }
  static auto Deserialize(BinaryReader&) -> Result<ClientRpcEnvelope> {
    return Error{ErrorCode::kInvalidArgument, "ClientRpcEnvelope is send-only"};
  }
};
static_assert(NetworkMessage<ClientRpcEnvelope>);

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_DELTA_FORWARDER_H_
