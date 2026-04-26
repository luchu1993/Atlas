#include "cell_entity.h"

#include <cfloat>
#include <utility>

#include "foundation/log.h"
#include "foundation/profiler.h"
#include "real_entity_data.h"
#include "space.h"
#include "witness.h"

namespace atlas {

CellEntity::CellEntity(EntityID id, uint16_t type_id, Space& space, const math::Vector3& position,
                       const math::Vector3& direction)
    : id_(id),
      type_id_(type_id),
      position_(position),
      direction_(direction),
      space_(space),
      range_node_(position.x, position.z) {
  // Stash the back-pointer so AoITrigger callbacks (which only get a
  // RangeListNode&) can recover the owning CellEntity via reinterpret_cast.
  range_node_.SetOwnerData(this);
  // Link the node into the owning RangeList now; SetPosition afterwards
  // will shuffle from this initial location. Insert does fire crosses
  // with any triggers already in the list at this position — that's the
  // "new entity joined" signal those triggers want.
  space_.GetRangeList().Insert(&range_node_);
  linked_to_range_list_ = true;
  // Default-construct the Real sidecar so IsReal() holds from the first
  // tick. Ghost construction uses the tag ctor below.
  real_data_ = std::make_unique<RealEntityData>(*this);
}

CellEntity::CellEntity(GhostTag, EntityID id, uint16_t type_id, Space& space,
                       const math::Vector3& position, const math::Vector3& direction,
                       Channel* real_channel)
    : id_(id),
      type_id_(type_id),
      position_(position),
      direction_(direction),
      space_(space),
      range_node_(position.x, position.z),
      real_channel_(real_channel) {
  // Ghost shares the same RangeList wiring as Real — cross-Cell observers'
  // Witnesses will pick it up through the ordinary AoITrigger machinery.
  range_node_.SetOwnerData(this);
  space_.GetRangeList().Insert(&range_node_);
  linked_to_range_list_ = true;
  // No RealEntityData allocation: the Real side lives elsewhere; our
  // cross-process bridge is real_channel_.
}

CellEntity::~CellEntity() {
  // Order:
  //   1. witness_.reset() — removes the AoITrigger's bound nodes while
  //      central is still linked.
  //   2. controllers_.StopAll() — ProximityController etc. hold their
  //      own RangeTriggers; StopAll removes those bound nodes cleanly.
  //   3. RangeList.Remove(&range_node_) — central leaves last.
  if (witness_) {
    witness_->Deactivate();
    witness_.reset();
  }
  controllers_.StopAll();

  if (linked_to_range_list_) {
    // Synthetic "vacate to infinity" shuffle BEFORE unlinking: sweeps
    // the range_node_ past every live trigger upper bound, firing
    // HandleCrossX/Z → DispatchMembership → OnLeave on any trigger
    // that had us in its inside_peers_ set. Without this, other
    // Witnesses' aoi_map_ entries (which hold raw CellEntity* to this
    // object) would keep dangling references after we're deleted, and
    // the next Witness::Update would UAF on our GetReplicationState().
    // RangeList::Remove itself doesn't fire cross events by design.
    const float old_x = range_node_.X();
    const float old_z = range_node_.Z();
    range_node_.SetXZ(FLT_MAX, FLT_MAX);
    space_.GetRangeList().ShuffleXThenZ(&range_node_, old_x, old_z);
    space_.GetRangeList().Remove(&range_node_);
    linked_to_range_list_ = false;
  }
}

void CellEntity::DisableWitness() {
  if (!witness_) return;
  witness_->Deactivate();
  witness_.reset();
}

void CellEntity::ConvertRealToGhost(Channel* new_real_channel) {
  if (!IsReal()) {
    ATLAS_LOG_WARNING("CellEntity::ConvertRealToGhost on non-Real entity id={} — ignored", id_);
    return;
  }
  // Tear down script-facing surfaces first so nothing fires during the
  // handover. Deactivate before reset to give AoITriggers a chance to
  // unhook.
  if (witness_) {
    witness_->Deactivate();
    witness_.reset();
  }
  controllers_.StopAll();
  // Drop the haunt list and velocity sample — we're no longer authoritative.
  real_data_.reset();
  real_channel_ = new_real_channel;
  // range_node_ stays in place; as a Ghost we're still spatially present
  // for peer witnesses observing this Cell.
}

void CellEntity::ConvertGhostToReal() {
  if (!IsGhost()) {
    ATLAS_LOG_WARNING("CellEntity::ConvertGhostToReal on non-Ghost entity id={} — ignored", id_);
    return;
  }
  real_channel_ = nullptr;
  next_real_addr_ = {};
  real_data_ = std::make_unique<RealEntityData>(*this);
  // replication_state_ carries over — the freshly-minted Real inherits
  // the baseline the Ghost was already serving until the next C# publish.
}

void CellEntity::GhostUpdatePosition(const math::Vector3& pos, const math::Vector3& direction,
                                     bool on_ground, uint64_t volatile_seq) {
  ATLAS_PROFILE_ZONE_N("CellEntity::GhostUpdatePosition");
  if (!IsGhost()) {
    ATLAS_LOG_WARNING("CellEntity::GhostUpdatePosition on non-Ghost entity id={} — ignored", id_);
    return;
  }
  if (!replication_state_.has_value()) replication_state_.emplace();
  auto& state = *replication_state_;
  // Latest-wins: drop stale / duplicate frames delivered out of order.
  if (volatile_seq <= state.latest_volatile_seq) return;
  state.latest_volatile_seq = volatile_seq;
  // SetPosition handles the RangeList shuffle; the Ghost version
  // deliberately reuses that path so peer Witness AoI stays consistent
  // whether the entity is Real or Ghost on this process.
  if (pos.x != position_.x || pos.y != position_.y || pos.z != position_.z) SetPosition(pos);
  direction_ = direction;
  on_ground_ = on_ground;
}

void CellEntity::GhostApplyDelta(uint64_t event_seq, std::span<const std::byte> other_delta) {
  ATLAS_PROFILE_ZONE_N("CellEntity::GhostApplyDelta");
  if (!IsGhost()) {
    ATLAS_LOG_WARNING("CellEntity::GhostApplyDelta on non-Ghost entity id={} — ignored", id_);
    return;
  }
  if (!replication_state_.has_value()) replication_state_.emplace();
  auto& state = *replication_state_;
  if (event_seq <= state.latest_event_seq) {
    ATLAS_LOG_DEBUG(
        "CellEntity::GhostApplyDelta: stale event_seq={} (latest={}) on entity id={} — dropped",
        event_seq, state.latest_event_seq, id_);
    return;
  }
  state.latest_event_seq = event_seq;
  // Parking the delta into history lets downstream Witnesses on this
  // CellApp catch up through the same replay path used for local
  // deltas, just with a cross-process origin.
  ReplicationFrame frame;
  frame.event_seq = event_seq;
  frame.other_delta.assign(other_delta.begin(), other_delta.end());
  frame.position = position_;
  frame.direction = direction_;
  frame.on_ground = on_ground_;
  state.history.push_back(std::move(frame));
  while (state.history.size() > kReplicationHistoryWindow) state.history.pop_front();
}

void CellEntity::RebindRealChannel(Channel* new_real_channel) {
  if (!IsGhost()) {
    ATLAS_LOG_WARNING("CellEntity::RebindRealChannel on non-Ghost id={} — ignored", id_);
    return;
  }
  real_channel_ = new_real_channel;
  // Offload handoff is done — the transition-window hint no longer applies.
  next_real_addr_ = {};
}

void CellEntity::GhostApplySnapshot(uint64_t event_seq, std::span<const std::byte> other_snapshot) {
  ATLAS_PROFILE_ZONE_N("CellEntity::GhostApplySnapshot");
  if (!IsGhost()) {
    ATLAS_LOG_WARNING("CellEntity::GhostApplySnapshot on non-Ghost entity id={} — ignored", id_);
    return;
  }
  if (!replication_state_.has_value()) replication_state_.emplace();
  auto& state = *replication_state_;
  // Snapshot refreshes fire when the sender detected a gap beyond the
  // history window, so they unconditionally reset the baseline —
  // history is useless once we've missed more than replay can cover.
  state.latest_event_seq = event_seq;
  state.other_snapshot.assign(other_snapshot.begin(), other_snapshot.end());
  state.history.clear();
}

void CellEntity::SetPosition(const math::Vector3& pos) {
  if (destroyed_) return;
  const math::Vector3 old = position_;
  position_ = pos;
  range_node_.SetXZ(pos.x, pos.z);
  space_.GetRangeList().ShuffleXThenZ(&range_node_, old.x, old.z);
}

void CellEntity::SetDirection(const math::Vector3& dir) {
  // Direction doesn't affect RangeList sort — no shuffle needed.
  direction_ = dir;
}

void CellEntity::SetPositionAndDirection(const math::Vector3& pos, const math::Vector3& dir) {
  if (destroyed_) return;
  const math::Vector3 old = position_;
  position_ = pos;
  direction_ = dir;
  range_node_.SetXZ(pos.x, pos.z);
  space_.GetRangeList().ShuffleXThenZ(&range_node_, old.x, old.z);
}

void CellEntity::PublishReplicationFrame(ReplicationFrame frame,
                                         std::span<const std::byte> owner_snapshot,
                                         std::span<const std::byte> other_snapshot) {
  ATLAS_PROFILE_ZONE_N("CellEntity::PublishReplicationFrame");
  if (!replication_state_.has_value()) replication_state_.emplace();
  auto& state = *replication_state_;

  // Event stream: ordered, cumulative. Advance + snapshot + history.
  if (frame.event_seq > state.latest_event_seq) {
    state.latest_event_seq = frame.event_seq;
    state.owner_snapshot.assign(owner_snapshot.begin(), owner_snapshot.end());
    state.other_snapshot.assign(other_snapshot.begin(), other_snapshot.end());
    state.history.push_back(frame);
    while (state.history.size() > kReplicationHistoryWindow) state.history.pop_front();
  }

  // Volatile stream: latest-wins. No history; just bump the counter and
  // adopt the new position/direction as C++'s authoritative mirror.
  if (frame.volatile_seq > state.latest_volatile_seq) {
    state.latest_volatile_seq = frame.volatile_seq;
    // The C# layer will have called atlas_set_position earlier in the
    // tick, so these fields may already agree; adopt unconditionally so
    // callers can publish without a prior SetPosition.
    if (frame.position.x != position_.x || frame.position.y != position_.y ||
        frame.position.z != position_.z) {
      SetPosition(frame.position);
    }
    direction_ = frame.direction;
    on_ground_ = frame.on_ground;
  }
}

auto CellEntity::GetReplicationState() const -> const ReplicationState* {
  return replication_state_.has_value() ? &*replication_state_ : nullptr;
}

void CellEntity::Destroy() {
  if (destroyed_) return;
  destroyed_ = true;
  // Actual resource cleanup (controllers, range_node_) happens in the
  // destructor. Space::RemoveEntity() erases the owning unique_ptr
  // shortly after, which triggers ~CellEntity in the correct order.
}

}  // namespace atlas
