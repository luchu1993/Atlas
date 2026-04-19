#include "space/range_trigger.h"

#include <cassert>

namespace atlas {

// ---------------------------------------------------------------------------
// RangeTriggerNode
// ---------------------------------------------------------------------------

RangeTriggerNode::RangeTriggerNode(RangeTrigger& owner, bool is_upper)
    : owner_(owner), is_upper_(is_upper) {
  // Bounds participate in the LowerAoi / UpperAoi flag vocabulary so the
  // RangeList only dispatches cross callbacks against peers that carry
  // kEntityTrigger (typically entities). Sentinel head/tail don't opt in,
  // so they cost nothing.
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

// ---------------------------------------------------------------------------
// RangeTrigger
// ---------------------------------------------------------------------------

RangeTrigger::RangeTrigger(RangeListNode& central, float range)
    : central_(central),
      range_(range),
      lower_bound_(*this, /*is_upper=*/false),
      upper_bound_(*this, /*is_upper=*/true) {
  assert(range >= 0.f && "RangeTrigger range must be non-negative");
}

RangeTrigger::~RangeTrigger() {
  // Safety net against leaks: if a caller drops us without first
  // calling Remove(), the embedded upper/lower bound nodes would stay
  // linked in the RangeList pointing at a destroyed trigger — the next
  // shuffle that crosses them would vcall into freed memory. The
  // canonical owner (Witness, ProximityController) does call Remove;
  // this protects against bugs like CellEntity::EnableWitness being
  // called twice without DisableWitness in between.
  if (list_ != nullptr) {
    list_->Remove(&upper_bound_);
    list_->Remove(&lower_bound_);
    list_ = nullptr;
  }
}

void RangeTrigger::Insert(RangeList& list) {
  assert(list_ == nullptr && "RangeTrigger already inserted");
  list_ = &list;
  // Insert order doesn't matter for correctness; inserting lower first
  // just reads more naturally in the list during debugging.
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
  // Seed is only meaningful while un-inserted: once the bounds are in
  // the list, Insert has already run and inside_peers_ reflects the
  // actual list state. Injecting after Insert would corrupt it.
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
  // Never fire for the central's own cross — central can cross its own
  // bounds when range shrinks past zero or during pathological input.
  if (&other == &central_) return;

  // Decoding the cross direction into "peer is now X-inside" helps
  // documentation, but ultimately we defer to the set-based state check
  // below — a cross alone is NOT proof of a 2-D transition (diagonal
  // inserts fire two crosses for one logical enter).
  const bool peer_now_x_inside = bound.IsUpper() ? /*upper*/ positive : /*lower*/ !positive;

  // Fast reject: if the peer's Z isn't in range at cross time, we know
  // the peer can't be 2-D-inside after this X-cross alone. A separate
  // later Z-cross (same tick) may flip 2-D state.
  const bool z_in_range = IsZInRange(other_z_at_cross);
  const bool peer_now_2d_inside = peer_now_x_inside && z_in_range;

  DispatchMembership(&other, peer_now_2d_inside);
}

void RangeTrigger::HandleCrossZ(RangeTriggerNode& bound, RangeListNode& other, bool positive,
                                float other_x_at_cross) {
  if (&other == &central_) return;

  const bool peer_now_z_inside = bound.IsUpper() ? /*upper*/ positive : /*lower*/ !positive;

  const bool x_in_range = IsXInRange(other_x_at_cross);
  const bool peer_now_2d_inside = peer_now_z_inside && x_in_range;

  DispatchMembership(&other, peer_now_2d_inside);
}

void RangeTrigger::DispatchMembership(RangeListNode* peer, bool now_inside) {
  // Set-based deduplication: fire OnEnter / OnLeave only on the true
  // transition edge. Inserts bubbling past both lower_x and lower_z
  // would otherwise emit two enters; diagonal traversals produce enter
  // + leave duplicates; this guard collapses them.
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

  // Determine which bound is "expanding" on X: moving away from central's
  // old position. Process it first to avoid spurious leave/enter events
  // when a peer sits exactly on the boundary.
  //
  // If central moved +dx, then:
  //   upper_bound.x was (old_cx + r), now (new_cx + r). Upper moved +dx.
  //                 That expands the RIGHT side of the box.
  //   lower_bound.x was (old_cx - r), now (new_cx - r). Lower moved +dx.
  //                 That contracts the LEFT side.
  // So when dx >= 0, we do upper first (expanding side). Symmetric for dx < 0.
  const float dx = central_.X() - old_central_x;
  (void)old_central_z;  // Z ordering is folded into each bound's ShuffleXThenZ
                        // call; see note in range_trigger.h.

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

  // "Expand first" again: pick the order that grows the box before
  // shrinking it so peers near the boundary get a clean enter before any
  // leave is fired on the opposite side.
  const float cx = central_.X();
  const float cz = central_.Z();
  const bool expanding = new_range > old_range;

  auto shuffle_bound = [this, cx, cz, old_range](RangeTriggerNode* bound) {
    const float sign = bound->IsUpper() ? 1.f : -1.f;
    // The bound's OLD list-sorted position is (cx + sign*old_range,
    // cz + sign*old_range) because central didn't move in SetRange.
    list_->ShuffleXThenZ(bound, cx + sign * old_range, cz + sign * old_range);
  };

  if (expanding) {
    // Both bounds expand outward. Either order works for enter-only
    // semantics; pick upper-first for consistency with Update(+dx).
    shuffle_bound(&upper_bound_);
    shuffle_bound(&lower_bound_);
  } else {
    // Contracting: do them outward-first is not possible (both contract
    // simultaneously). Order doesn't matter here — at most we get a
    // slightly different sequence of OnLeave calls.
    shuffle_bound(&upper_bound_);
    shuffle_bound(&lower_bound_);
  }
}

}  // namespace atlas
