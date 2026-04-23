#ifndef ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_
#define ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cellapp/intercell_messages.h"
#include "foundation/clock.h"
#include "math/vector3.h"
#include "network/address.h"

namespace atlas {

class CellEntity;
class Channel;

// RealEntityData — distributed-state sidecar for a Real CellEntity.
// Owns only Real-only data: the Haunt list of peer CellApps currently
// holding a Ghost replica, a velocity sample used by the offload
// checker, and per-haunt broadcast bookkeeping. Everything else
// (witness, controllers, replication state, range_node_) stays on
// CellEntity so the destruction order remains intact.
//
// Lifetime: constructed by CellEntity's Real ctor; destroyed when the
// entity is destroyed OR when it transitions Real → Ghost (which drops
// the sidecar). None of the Channel* pointers in Haunt are owned.

class RealEntityData {
 public:
  struct Haunt {
    Channel* channel{nullptr};
    Address addr{};
    TimePoint creation_time{};
  };

  explicit RealEntityData(CellEntity& owner);
  ~RealEntityData();

  RealEntityData(const RealEntityData&) = delete;
  auto operator=(const RealEntityData&) -> RealEntityData& = delete;

  // ---- Haunt management ----------------------------------------------------

  // Idempotent: adding a Channel already in the Haunt list is a no-op.
  // Returns true when a new haunt was recorded, false when the peer was
  // already present. `addr` is cached on the Haunt so GhostMaintainer can
  // decide keep-vs-delete by Address identity — robust against peer
  // reconnects that rebind the Channel* while the Address is unchanged.
  auto AddHaunt(Channel* channel, const Address& addr) -> bool;

  // Returns true when a matching haunt was erased.
  auto RemoveHaunt(Channel* channel) -> bool;

  [[nodiscard]] auto HasHaunt(Channel* channel) const -> bool;
  [[nodiscard]] auto HauntCount() const -> std::size_t { return haunts_.size(); }
  [[nodiscard]] auto Haunts() const -> const std::vector<Haunt>& { return haunts_; }
  [[nodiscard]] auto Haunts() -> std::vector<Haunt>& { return haunts_; }

  // ---- Message builders ---------------------------------------------------
  //
  // CellApp drives the send side: it iterates Haunts() and calls
  // channel->SendMessage with the payload the builder returned. Keeping
  // RealEntityData passive lets unit tests verify message contents
  // without any networking harness.
  //
  // All three builders read from the owner's ReplicationState — the
  // owner_snapshot/other_snapshot + latest_*_seq that Witness already
  // consumes for client-facing delivery. Ghosts end up seeing the exact
  // same "other" audience projection the client would have, just shipped
  // cross-process.

  [[nodiscard]] auto BuildPositionUpdate() const -> cellapp::GhostPositionUpdate;
  [[nodiscard]] auto BuildDelta() const -> cellapp::GhostDelta;
  [[nodiscard]] auto BuildSnapshotRefresh() const -> cellapp::GhostSnapshotRefresh;

  // Decision helper for the ghost-pump broadcast. Returns true when the
  // gap between `latest_event_seq` and `last_broadcast_event_seq` means
  // BuildDelta would lose intermediate frames (gap > 1) — the pump
  // should issue a GhostSnapshotRefresh instead. Returns false for
  // gap ≤ 1 (BuildDelta covers the single frame) or gap == 0 (no
  // broadcast needed). Atlas batches per tick, so this helper handles
  // the batching case explicitly.
  [[nodiscard]] static constexpr auto ShouldUseSnapshotRefresh(uint64_t latest_event_seq,
                                                               uint64_t last_broadcast_event_seq)
      -> bool {
    return latest_event_seq > last_broadcast_event_seq + 1;
  }

  // Bandwidth optimisation for the gap==1 broadcast case: if the delta
  // carries no audience-visible content (either empty, or the
  // DeltaSyncEmitter flag prefix with all zeros — which happens when
  // only owner-visible properties were dirty), skip the SendMessage.
  // The Real still advances last_broadcast_event_seq_ so the next tick's
  // gap arithmetic stays correct; the Ghost's seq catches up naturally
  // on the next non-empty delta. Safe because an all-zero payload
  // trivially has no field-update content to deliver — any dirty field
  // would set at least one flag bit.
  [[nodiscard]] static auto IsEmptyOtherDelta(std::span<const std::byte> delta) -> bool {
    return std::all_of(delta.begin(), delta.end(), [](std::byte b) { return b == std::byte{0}; });
  }

  // ---- Broadcast bookkeeping ----------------------------------------------
  //
  // CellApp's per-tick ghost pump compares the owner's latest_*_seq
  // against these to decide whether to broadcast again. The last-sent
  // values are updated only by CellApp after SendMessage succeeds — this
  // module is agnostic to network errors.

  [[nodiscard]] auto LastBroadcastEventSeq() const -> uint64_t { return last_broadcast_event_seq_; }
  [[nodiscard]] auto LastBroadcastVolatileSeq() const -> uint64_t {
    return last_broadcast_volatile_seq_;
  }
  void MarkBroadcastEventSeq(uint64_t seq) { last_broadcast_event_seq_ = seq; }
  void MarkBroadcastVolatileSeq(uint64_t seq) { last_broadcast_volatile_seq_ = seq; }

  // Timestamp of the last ghost broadcast (any kind). The ghost pump
  // uses this to throttle per-entity fan-out to the configured update
  // interval, so a C#-driven position change every tick doesn't turn
  // into a per-tick fan-out to every Haunt.
  [[nodiscard]] auto LastBroadcastTime() const -> TimePoint { return last_broadcast_time_; }
  void MarkBroadcastTime(TimePoint t) { last_broadcast_time_ = t; }

  // ---- Velocity estimation ------------------------------------------------
  //
  // OffloadChecker reads velocity to project whether the entity is about
  // to cross a Cell border and should be handed off early. dt is in
  // seconds; call once per tick with the entity's current position.

  [[nodiscard]] auto Velocity() const -> const math::Vector3& { return velocity_; }
  void UpdateVelocity(const math::Vector3& new_pos, float dt);

 private:
  CellEntity& owner_;
  std::vector<Haunt> haunts_;
  math::Vector3 velocity_{};
  math::Vector3 position_sample_{};
  bool has_position_sample_{false};

  // The most recently broadcast-confirmed Real-side sequences. Lets CellApp
  // compare against owner's current ReplicationState and decide whether
  // to (a) skip broadcasting this tick, (b) broadcast a fresh delta, or
  // (c) fall back to a snapshot refresh when the gap exceeds the
  // history window.
  uint64_t last_broadcast_event_seq_{0};
  uint64_t last_broadcast_volatile_seq_{0};
  TimePoint last_broadcast_time_{};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_
