#include "space/range_trigger.h"

#include <cassert>

namespace atlas {

RangeTriggerNode::RangeTriggerNode(RangeTrigger& owner, bool is_upper)
    : owner_(owner), is_upper_(is_upper) {
  makes_flags_ = is_upper_ ? RangeListFlags::kUpperAoiTrigger : RangeListFlags::kLowerAoiTrigger;
  wants_flags_ = RangeListFlags::kEntityTrigger;
}

auto RangeTriggerNode::X() const -> float {
  const float r = owner_.Range();
  return owner_.Central().X() + (is_upper_ ? r : -r);
}

auto RangeTriggerNode::Z() const -> float {
  const float r = owner_.Range();
  return owner_.Central().Z() + (is_upper_ ? r : -r);
}

auto RangeTriggerNode::Order() const -> RangeListOrder {
  return is_upper_ ? RangeListOrder::kUpperBound : RangeListOrder::kLowerBound;
}

void RangeTriggerNode::OnCrossedX(RangeListNode& other, bool positive, float other_ortho) {
  owner_.HandleCrossX(*this, other, positive, other_ortho);
}

void RangeTriggerNode::OnCrossedZ(RangeListNode& other, bool positive, float other_ortho) {
  owner_.HandleCrossZ(*this, other, positive, other_ortho);
}

RangeTrigger::RangeTrigger(RangeListNode& central, float range)
    : central_(central),
      range_(range),
      lower_bound_(*this, /*is_upper=*/false),
      upper_bound_(*this, /*is_upper=*/true) {
  assert(range >= 0.f && "RangeTrigger range must be non-negative");
}

RangeTrigger::~RangeTrigger() {
  // Bound nodes store a back-pointer to this trigger; never leave them linked.
  if (list_ != nullptr) {
    list_->Remove(&upper_bound_);
    list_->Remove(&lower_bound_);
    list_ = nullptr;
  }
}

void RangeTrigger::Insert(RangeList& list) {
  assert(list_ == nullptr && "RangeTrigger already inserted");
  list_ = &list;
  list.Insert(&lower_bound_);
  list.Insert(&upper_bound_);
}

void RangeTrigger::Remove(RangeList& list) {
  assert(list_ == &list);
  list.Remove(&upper_bound_);
  list.Remove(&lower_bound_);
  list_ = nullptr;
}

void RangeTrigger::SeedInsidePeersForMigration(std::unordered_set<RangeListNode*> peers) {
  assert(list_ == nullptr && "SeedInsidePeersForMigration after Insert()");
  inside_peers_ = std::move(peers);
}

auto RangeTrigger::IsZInRange(float z) const -> bool {
  const float cz = central_.Z();
  return z > (cz - range_) && z < (cz + range_);
}

auto RangeTrigger::IsXInRange(float x) const -> bool {
  const float cx = central_.X();
  return x > (cx - range_) && x < (cx + range_);
}

void RangeTrigger::HandleCrossX(RangeTriggerNode& bound, RangeListNode& other, bool positive,
                                float other_z_at_cross) {
  if (&other == &central_) return;

  const bool peer_now_x_inside = bound.IsUpper() ? positive : !positive;

  const bool z_in_range = IsZInRange(other_z_at_cross);
  const bool peer_now_2d_inside = peer_now_x_inside && z_in_range;

  DispatchMembership(&other, peer_now_2d_inside);
}

void RangeTrigger::HandleCrossZ(RangeTriggerNode& bound, RangeListNode& other, bool positive,
                                float other_x_at_cross) {
  if (&other == &central_) return;

  const bool peer_now_z_inside = bound.IsUpper() ? positive : !positive;

  const bool x_in_range = IsXInRange(other_x_at_cross);
  const bool peer_now_2d_inside = peer_now_z_inside && x_in_range;

  DispatchMembership(&other, peer_now_2d_inside);
}

void RangeTrigger::DispatchMembership(RangeListNode* peer, bool now_inside) {
  const bool was_inside = inside_peers_.count(peer) > 0;
  if (now_inside == was_inside) return;

  if (now_inside) {
    inside_peers_.insert(peer);
    OnEnter(*peer);
  } else {
    inside_peers_.erase(peer);
    OnLeave(*peer);
  }
}

void RangeTrigger::Update(float old_central_x, float old_central_z) {
  assert(list_ != nullptr && "RangeTrigger::Update before Insert");

  // Process the expanding X bound first to avoid boundary leave/enter chatter.
  const float dx = central_.X() - old_central_x;
  (void)old_central_z;

  auto shuffle_bound = [this, old_central_x, old_central_z](RangeTriggerNode* bound) {
    const float sign = bound->IsUpper() ? 1.f : -1.f;
    const float old_bound_x = old_central_x + sign * range_;
    const float old_bound_z = old_central_z + sign * range_;
    list_->ShuffleXThenZ(bound, old_bound_x, old_bound_z);
  };

  if (dx >= 0.f) {
    shuffle_bound(&upper_bound_);
    shuffle_bound(&lower_bound_);
  } else {
    shuffle_bound(&lower_bound_);
    shuffle_bound(&upper_bound_);
  }
}

void RangeTrigger::SetRange(float new_range) {
  assert(new_range >= 0.f && "SetRange: range must be non-negative");
  assert(list_ != nullptr && "SetRange before Insert");

  const float old_range = range_;
  range_ = new_range;

  const float cx = central_.X();
  const float cz = central_.Z();
  const bool expanding = new_range > old_range;

  auto shuffle_bound = [this, cx, cz, old_range](RangeTriggerNode* bound) {
    const float sign = bound->IsUpper() ? 1.f : -1.f;
    list_->ShuffleXThenZ(bound, cx + sign * old_range, cz + sign * old_range);
  };

  if (expanding) {
    shuffle_bound(&upper_bound_);
    shuffle_bound(&lower_bound_);
  } else {
    shuffle_bound(&upper_bound_);
    shuffle_bound(&lower_bound_);
  }
}

}  // namespace atlas
