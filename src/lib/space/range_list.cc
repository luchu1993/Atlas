#include "space/range_list.h"

#include <cassert>

namespace atlas {

namespace {

// Project a node's Order() to a uint16 for the tie-break comparisons.
// Having it inline lets both shuffle loops stay branch-light.
constexpr auto OrderValue(const RangeListNode& n) -> uint16_t {
  return static_cast<uint16_t>(n.Order());
}

// Unlink a node from its X-axis neighbours (prev_x / next_x must be set).
void UnlinkX(RangeListNode* node) {
  node->prev_x_->next_x_ = node->next_x_;
  node->next_x_->prev_x_ = node->prev_x_;
}

// Unlink from Z-axis neighbours.
void UnlinkZ(RangeListNode* node) {
  node->prev_z_->next_z_ = node->next_z_;
  node->next_z_->prev_z_ = node->prev_z_;
}

// Insert `n` between `before` and `before->next_x_` on the X list.
void InsertAfterX(RangeListNode* before, RangeListNode* n) {
  n->prev_x_ = before;
  n->next_x_ = before->next_x_;
  before->next_x_->prev_x_ = n;
  before->next_x_ = n;
}

// Insert `n` between `before` and `before->next_z_` on the Z list.
void InsertAfterZ(RangeListNode* before, RangeListNode* n) {
  n->prev_z_ = before;
  n->next_z_ = before->next_z_;
  before->next_z_->prev_z_ = n;
  before->next_z_ = n;
}

// Selective cross-notification helper: only dispatch if `a` and `b` have
// opted in via their flag bit-sets. This keeps the hot path free of
// virtual calls for nodes that don't care (e.g. the fixed sentinels).
auto ShouldNotify(const RangeListNode& a, const RangeListNode& b) -> bool {
  return Any(a.MakesFlags() & b.WantsFlags()) || Any(b.MakesFlags() & a.WantsFlags());
}

}  // namespace

RangeList::RangeList() {
  // Both sentinels form a list of size-2 on each axis; shuffle/insert
  // loops can thus walk prev/next without guard conditions.
  head_.next_x_ = &tail_;
  head_.next_z_ = &tail_;
  tail_.prev_x_ = &head_;
  tail_.prev_z_ = &head_;
}

void RangeList::Insert(RangeListNode* node) {
  assert(node != nullptr);
  assert(node->prev_x_ == nullptr && node->next_x_ == nullptr &&
         "RangeList::Insert called on an already-linked node");

  // Drop in at the head sentinel's right neighbour, then let shuffle
  // bubble the node to its true position. The sentinel declares no flags
  // so cross callbacks stay silent against head/tail; whether a cross
  // fires between the new node and existing live nodes is a legitimate
  // AoI "entity-join" signal the trigger layer consumes.
  InsertAfterX(&head_, node);
  InsertAfterZ(&head_, node);
  // For inserts we pass the node's OWN current Z/X as the "old" value —
  // the node hasn't moved within a shuffle context, so the orthogonal
  // coord a trigger would want is just the current position.
  ShuffleX(node, node->Z());
  ShuffleZ(node, node->X());
}

void RangeList::Remove(RangeListNode* node) {
  assert(node != nullptr);
  assert(node->prev_x_ != nullptr && node->next_x_ != nullptr);
  UnlinkX(node);
  UnlinkZ(node);
  node->prev_x_ = node->next_x_ = nullptr;
  node->prev_z_ = node->next_z_ = nullptr;
}

void RangeList::ShuffleXThenZ(RangeListNode* node, float old_x, float old_z) {
  // Order matters: the X shuffle uses the node's OLD z to decide 2-D
  // crossing direction, then the Z shuffle uses the node's NEW (already
  // shuffled) x. This two-phase dance is what makes RangeTrigger's
  // rectangular membership queries correct.
  ShuffleX(node, old_z);
  ShuffleZ(node, node->X());
  (void)old_x;  // Kept in the API for symmetry; may be needed if a Z-first
                // ordering ever surfaces (e.g. for test replay). Unused today.
}

void RangeList::ShuffleX(RangeListNode* node, float old_z_of_node) {
  const float ours = node->X();
  const auto our_order = OrderValue(*node);

  // Bubble left while our key is strictly smaller than prev. The
  // tie-break on Order prevents UpperBound/Entity/LowerBound from
  // oscillating when all three share an x coordinate.
  while (true) {
    auto* prev = node->prev_x_;
    const float prev_x = prev->X();
    if (!(ours < prev_x || (ours == prev_x && our_order < OrderValue(*prev)))) break;

    if (ShouldNotify(*node, *prev)) {
      // `node` moved past `prev` going LEFT — from node's perspective
      // it's now BELOW prev (x-wise); from prev's perspective it's now
      // ABOVE node. The orthogonal coord we surface is the COUNTERPART's
      // Z at cross time: for the stationary prev that's prev.Z(); for
      // the moving `node` whose Z hasn't shuffled yet that's the
      // caller-supplied `old_z_of_node`.
      node->OnCrossedX(*prev, /*positive=*/false, /*other_ortho=*/prev->Z());
      prev->OnCrossedX(*node, /*positive=*/true, /*other_ortho=*/old_z_of_node);
    }

    // Swap by unlink + reinsert; this is a 4-pointer shuffle.
    UnlinkX(node);
    InsertAfterX(prev->prev_x_, node);
  }

  // Mirror image: bubble right while our key is strictly greater than next.
  while (true) {
    auto* next = node->next_x_;
    const float next_x = next->X();
    if (!(ours > next_x || (ours == next_x && our_order > OrderValue(*next)))) break;

    if (ShouldNotify(*node, *next)) {
      node->OnCrossedX(*next, /*positive=*/true, /*other_ortho=*/next->Z());
      next->OnCrossedX(*node, /*positive=*/false, /*other_ortho=*/old_z_of_node);
    }

    UnlinkX(node);
    InsertAfterX(next, node);
  }
}

void RangeList::ShuffleZ(RangeListNode* node, float old_x_of_node) {
  const float ours = node->Z();
  const auto our_order = OrderValue(*node);

  while (true) {
    auto* prev = node->prev_z_;
    const float prev_z = prev->Z();
    if (!(ours < prev_z || (ours == prev_z && our_order < OrderValue(*prev)))) break;

    if (ShouldNotify(*node, *prev)) {
      // ShuffleZ runs AFTER ShuffleX, so `node`'s X has already been
      // updated. `old_x_of_node` therefore equals node->X() at this
      // point — we pass it explicitly to keep the symmetry with ShuffleX
      // and to document the convention.
      node->OnCrossedZ(*prev, /*positive=*/false, /*other_ortho=*/prev->X());
      prev->OnCrossedZ(*node, /*positive=*/true, /*other_ortho=*/old_x_of_node);
    }

    UnlinkZ(node);
    InsertAfterZ(prev->prev_z_, node);
  }

  while (true) {
    auto* next = node->next_z_;
    const float next_z = next->Z();
    if (!(ours > next_z || (ours == next_z && our_order > OrderValue(*next)))) break;

    if (ShouldNotify(*node, *next)) {
      node->OnCrossedZ(*next, /*positive=*/true, /*other_ortho=*/next->X());
      next->OnCrossedZ(*node, /*positive=*/false, /*other_ortho=*/old_x_of_node);
    }

    UnlinkZ(node);
    InsertAfterZ(next, node);
  }
}

}  // namespace atlas
