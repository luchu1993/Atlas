#include "space/range_list.h"

#include <cassert>

namespace atlas {

namespace {

constexpr auto OrderValue(const RangeListNode& n) -> uint16_t {
  return static_cast<uint16_t>(n.Order());
}

void UnlinkX(RangeListNode* node) {
  node->prev_x_->next_x_ = node->next_x_;
  node->next_x_->prev_x_ = node->prev_x_;
}

void UnlinkZ(RangeListNode* node) {
  node->prev_z_->next_z_ = node->next_z_;
  node->next_z_->prev_z_ = node->prev_z_;
}

void InsertAfterX(RangeListNode* before, RangeListNode* n) {
  n->prev_x_ = before;
  n->next_x_ = before->next_x_;
  before->next_x_->prev_x_ = n;
  before->next_x_ = n;
}

void InsertAfterZ(RangeListNode* before, RangeListNode* n) {
  n->prev_z_ = before;
  n->next_z_ = before->next_z_;
  before->next_z_->prev_z_ = n;
  before->next_z_ = n;
}

auto ShouldNotify(const RangeListNode& a, const RangeListNode& b) -> bool {
  return Any(a.MakesFlags() & b.WantsFlags()) || Any(b.MakesFlags() & a.WantsFlags());
}

}  // namespace

RangeList::RangeList() {
  head_.next_x_ = &tail_;
  head_.next_z_ = &tail_;
  tail_.prev_x_ = &head_;
  tail_.prev_z_ = &head_;
}

void RangeList::Insert(RangeListNode* node) {
  assert(node != nullptr);
  assert(node->prev_x_ == nullptr && node->next_x_ == nullptr &&
         "RangeList::Insert called on an already-linked node");

  InsertAfterX(&head_, node);
  InsertAfterZ(&head_, node);
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
  // X first, then Z, so rectangular membership sees the right orthogonal coord.
  ShuffleX(node, old_z);
  ShuffleZ(node, node->X());
  (void)old_x;
}

void RangeList::ShuffleX(RangeListNode* node, float old_z_of_node) {
  const float ours = node->X();
  const auto our_order = OrderValue(*node);

  while (true) {
    auto* prev = node->prev_x_;
    const float prev_x = prev->X();
    if (!(ours < prev_x || (ours == prev_x && our_order < OrderValue(*prev)))) break;

    if (ShouldNotify(*node, *prev)) {
      node->OnCrossedX(*prev, /*positive=*/false, /*other_ortho=*/prev->Z());
      prev->OnCrossedX(*node, /*positive=*/true, /*other_ortho=*/old_z_of_node);
    }

    UnlinkX(node);
    InsertAfterX(prev->prev_x_, node);
  }

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
