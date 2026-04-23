#include "real_entity_data.h"

#include <algorithm>

#include "cell_entity.h"
#include "foundation/log.h"

namespace atlas {

RealEntityData::RealEntityData(CellEntity& owner) : owner_(owner) {}
RealEntityData::~RealEntityData() = default;

// ---- Haunt list ------------------------------------------------------------

auto RealEntityData::AddHaunt(Channel* channel, const Address& addr) -> bool {
  if (channel == nullptr) return false;
  if (HasHaunt(channel)) return false;
  haunts_.push_back(Haunt{channel, addr, Clock::now()});
  return true;
}

auto RealEntityData::RemoveHaunt(Channel* channel) -> bool {
  auto it = std::find_if(haunts_.begin(), haunts_.end(),
                         [channel](const Haunt& h) { return h.channel == channel; });
  if (it == haunts_.end()) return false;
  // Swap-back keeps the vector O(1) at the cost of ordering — callers of
  // Haunts() don't rely on creation order (iteration target is fan-out,
  // not sequence).
  *it = haunts_.back();
  haunts_.pop_back();
  return true;
}

auto RealEntityData::HasHaunt(Channel* channel) const -> bool {
  return std::any_of(haunts_.begin(), haunts_.end(),
                     [channel](const Haunt& h) { return h.channel == channel; });
}

// ---- Message builders ------------------------------------------------------

auto RealEntityData::BuildPositionUpdate() const -> cellapp::GhostPositionUpdate {
  cellapp::GhostPositionUpdate msg;
  msg.ghost_entity_id = owner_.Id();
  msg.position = owner_.Position();
  msg.direction = owner_.Direction();
  msg.on_ground = owner_.OnGround();
  const auto* state = owner_.GetReplicationState();
  msg.volatile_seq = state ? state->latest_volatile_seq : 0;
  return msg;
}

auto RealEntityData::BuildDelta() const -> cellapp::GhostDelta {
  cellapp::GhostDelta msg;
  msg.ghost_entity_id = owner_.Id();
  const auto* state = owner_.GetReplicationState();
  if (state == nullptr || state->history.empty()) return msg;

  // Invariant: PublishReplicationFrame only appends to `history` when it
  // also advances `latest_event_seq`, so the back frame's seq should
  // always equal the current latest. If they diverge, forwarding back()'s
  // delta bytes with a mismatched seq would hand the Ghost a corrupted
  // record. Prefer to ship an empty delta so the pump's gap > 1 check
  // on the next tick promotes the refresh to a GhostSnapshotRefresh.
  const auto& latest = state->history.back();
  if (latest.event_seq != state->latest_event_seq) {
    ATLAS_LOG_WARNING(
        "RealEntityData::BuildDelta: history.back().event_seq={} mismatches "
        "state->latest_event_seq={} for entity id={} — shipping empty delta; "
        "next pump will upgrade to a snapshot refresh",
        latest.event_seq, state->latest_event_seq, owner_.Id());
    msg.event_seq = state->latest_event_seq;
    return msg;
  }
  msg.event_seq = state->latest_event_seq;
  msg.other_delta = latest.other_delta;
  return msg;
}

auto RealEntityData::BuildSnapshotRefresh() const -> cellapp::GhostSnapshotRefresh {
  cellapp::GhostSnapshotRefresh msg;
  msg.ghost_entity_id = owner_.Id();
  const auto* state = owner_.GetReplicationState();
  if (state) {
    msg.event_seq = state->latest_event_seq;
    msg.other_snapshot = state->other_snapshot;
  }
  return msg;
}

// ---- Velocity --------------------------------------------------------------

void RealEntityData::UpdateVelocity(const math::Vector3& new_pos, float dt) {
  if (!has_position_sample_ || dt <= 0.f) {
    position_sample_ = new_pos;
    has_position_sample_ = true;
    velocity_ = {0.f, 0.f, 0.f};
    return;
  }
  const math::Vector3 delta{new_pos.x - position_sample_.x, new_pos.y - position_sample_.y,
                            new_pos.z - position_sample_.z};
  velocity_ = {delta.x / dt, delta.y / dt, delta.z / dt};
  position_sample_ = new_pos;
}

}  // namespace atlas
