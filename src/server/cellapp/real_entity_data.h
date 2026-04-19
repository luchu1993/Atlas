#ifndef ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_
#define ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cellapp/intercell_messages.h"
#include "foundation/clock.h"
#include "math/vector3.h"

namespace atlas {

class CellEntity;
class Channel;

// ============================================================================
// RealEntityData — distributed-state sidecar for a Real CellEntity.
//
// Phase 11 §3.2. Owns only the data that is Real-only: the Haunt list that
// tracks every peer CellApp currently holding a Ghost replica, a velocity
// sample used by the offload checker, and per-haunt broadcast bookkeeping.
// Everything else — witness, controllers, replication state, range_node_ —
// stays on CellEntity so the Phase 10 destruction order (§3.10 #4) remains
// intact.
//
// Lifetime: constructed by CellEntity's Real ctor; destroyed when the
// entity is destroyed OR when it transitions Real → Ghost (which drops
// the sidecar). None of the Channel* pointers in Haunt are owned.
// ============================================================================

class RealEntityData {
 public:
  struct Haunt {
    Channel* channel{nullptr};
    TimePoint creation_time{};
  };

  explicit RealEntityData(CellEntity& owner);
  ~RealEntityData();

  RealEntityData(const RealEntityData&) = delete;
  auto operator=(const RealEntityData&) -> RealEntityData& = delete;

  // ---- Haunt management ----------------------------------------------------

  // Idempotent: adding a Channel already in the Haunt list is a no-op.
  // Returns true when a new haunt was recorded, false when the peer was
  // already present.
  auto AddHaunt(Channel* channel) -> bool;

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
  // without any networking harness (PR-3 tests do exactly that).
  //
  // All three builders read from the owner's Phase 10 ReplicationState —
  // the owner_snapshot/other_snapshot + latest_*_seq that Witness already
  // consumes for client-facing delivery. Ghosts end up seeing the exact
  // same "other" audience projection the client would have, just
  // shipped cross-process.

  [[nodiscard]] auto BuildPositionUpdate() const -> cellapp::GhostPositionUpdate;
  [[nodiscard]] auto BuildDelta() const -> cellapp::GhostDelta;
  [[nodiscard]] auto BuildSnapshotRefresh() const -> cellapp::GhostSnapshotRefresh;

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

  // ---- Velocity estimation ------------------------------------------------
  //
  // OffloadChecker reads velocity to project whether the entity is about
  // to cross a Cell border and should be handed off early (Phase 11
  // §3.4). dt is in seconds; call once per tick with the entity's current
  // position.

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
  // (c) fall back to a snapshot refresh when the gap exceeds the Phase
  // 10 history window.
  uint64_t last_broadcast_event_seq_{0};
  uint64_t last_broadcast_volatile_seq_{0};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_
