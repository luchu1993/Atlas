#include "bsp_tree.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace atlas {

auto BSPLeaf::FindCell(float x, float z) const -> const CellInfo* {
  return info_.bounds.Contains(x, z) ? &info_ : nullptr;
}

auto BSPLeaf::FindPrimaryCell(float x, float z, float /*ghost_region*/) const -> const CellInfo* {
  // Reaching a leaf means every ancestor split's ghost band was cleared.
  return info_.bounds.Contains(x, z) ? &info_ : nullptr;
}

void BSPLeaf::VisitRect(const CellBounds& rect,
                        const std::function<void(const CellInfo&)>& visitor) const {
  if (info_.bounds.Overlaps(rect)) visitor(info_);
}

void BSPLeaf::PropagateBounds(const CellBounds& sub_bounds) {
  info_.bounds = sub_bounds;
}

void BSPLeaf::UpdateLoad() {}

void BSPLeaf::Serialize(BinaryWriter& w) const {
  w.Write(static_cast<uint8_t>(0));  // type tag: 0 = leaf
  w.Write(info_.cell_id);
  w.Write(info_.cellapp_addr.Ip());
  w.Write(info_.cellapp_addr.Port());
}

auto BSPInternal::FindCell(float x, float z) const -> const CellInfo* {
  // Half-open on position_ to match CellBounds::Contains.
  const float coord = (axis_ == BSPAxis::kX) ? x : z;
  return coord < position_ ? left_->FindCell(x, z) : right_->FindCell(x, z);
}

auto BSPInternal::FindPrimaryCell(float x, float z, float ghost_region) const -> const CellInfo* {
  const float coord = (axis_ == BSPAxis::kX) ? x : z;
  if (coord < position_ - ghost_region) return left_->FindPrimaryCell(x, z, ghost_region);
  if (coord > position_ + ghost_region) return right_->FindPrimaryCell(x, z, ghost_region);
  return nullptr;  // inside this split's ghost band — defer migration
}

void BSPInternal::VisitRect(const CellBounds& rect,
                            const std::function<void(const CellInfo&)>& visitor) const {
  // Descend into whichever side the rect touches. Common case (rect
  // strictly inside one side) visits only that side; straddling visits
  // both.
  if (axis_ == BSPAxis::kX) {
    if (rect.min_x < position_) left_->VisitRect(rect, visitor);
    if (rect.max_x > position_) right_->VisitRect(rect, visitor);
  } else {
    if (rect.min_z < position_) left_->VisitRect(rect, visitor);
    if (rect.max_z > position_) right_->VisitRect(rect, visitor);
  }
}

void BSPInternal::PropagateBounds(const CellBounds& sub_bounds) {
  sub_bounds_ = sub_bounds;
  CellBounds left_b = sub_bounds;
  CellBounds right_b = sub_bounds;
  if (axis_ == BSPAxis::kX) {
    left_b.max_x = position_;
    right_b.min_x = position_;
  } else {
    left_b.max_z = position_;
    right_b.min_z = position_;
  }
  left_->PropagateBounds(left_b);
  right_->PropagateBounds(right_b);
}

void BSPInternal::UpdateLoad() {
  left_->UpdateLoad();
  right_->UpdateLoad();
  left_load_ = left_->TotalLoad();
  right_load_ = right_->TotalLoad();
}

void BSPInternal::Balance(float safety_bound) {
  // Compare sibling loads, decide direction, apply aggression damping,
  // then shift position_. Leaves themselves are no-ops but the new
  // split line has to be pushed back down via PropagateBounds after
  // all internal nodes in this subtree have settled.
  // Hysteresis at 0.01f suppresses jitter when the tree is already
  // balanced.
  constexpr float kLoadHysteresis = 0.01f;
  const float diff = left_load_ - right_load_;
  Direction d = Direction::kNone;
  if (diff > kLoadHysteresis)
    d = Direction::kLeft;
  else if (diff < -kLoadHysteresis)
    d = Direction::kRight;

  if (d != Direction::kNone) {
    // Don't make the already-growing side worse - if the side that WOULD
    // grow (i.e. the opposite of the heavy side) is already over
    // safety_bound, hold still.
    const float growing_side_load = (d == Direction::kLeft) ? right_load_ : left_load_;
    if (growing_side_load < safety_bound) {
      if (d != prev_direction_ && prev_direction_ != Direction::kNone) {
        aggression_ *= 0.9f;  // direction reversed - damp
      } else {
        aggression_ = std::min(aggression_ * 1.1f, 2.0f);  // keep going - accelerate
      }
      prev_direction_ = d;

      // Move 10% of the imbalance x aggression. Sign follows `diff`: when
      // diff > 0 (left heavier), position_ increases -> shrinks left.
      const float move = diff * 0.1f * aggression_;
      const float new_position = position_ + move;

      // Clamp the split into our sub_bounds so we never produce an empty
      // half. A zero-volume half would trap entities on whichever side
      // they happened to be and starve Balance on the next pass.
      const float lo = (axis_ == BSPAxis::kX) ? sub_bounds_.min_x : sub_bounds_.min_z;
      const float hi = (axis_ == BSPAxis::kX) ? sub_bounds_.max_x : sub_bounds_.max_z;
      if (std::isfinite(lo) && std::isfinite(hi) && lo < hi) {
        // Leave a 1% slack at each end to ensure strictly-inside clamp.
        const float span = hi - lo;
        const float pad = span * 0.01f;
        position_ = std::clamp(new_position, lo + pad, hi - pad);
      } else {
        position_ = new_position;
      }

      // Re-propagate bounds so descendant leaves reflect the new split.
      PropagateBounds(sub_bounds_);
    }
  }

  // Recurse so nested internals can balance within their new sub-bounds.
  left_->Balance(safety_bound);
  right_->Balance(safety_bound);
}

void BSPInternal::Serialize(BinaryWriter& w) const {
  w.Write(static_cast<uint8_t>(1));  // type tag: 1 = internal
  w.Write(static_cast<uint8_t>(axis_));
  w.Write(position_);
  left_->Serialize(w);
  right_->Serialize(w);
}

namespace {

auto DeserializeNode(BinaryReader& r) -> Result<std::unique_ptr<BSPNode>> {
  auto tag = r.Read<uint8_t>();
  if (!tag) return Error{ErrorCode::kInvalidArgument, "BSPNode: truncated tag"};
  if (*tag == 0) {
    auto cid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    if (!cid || !ip || !port) return Error{ErrorCode::kInvalidArgument, "BSPLeaf: truncated"};
    CellInfo info;
    info.cell_id = *cid;
    info.cellapp_addr = Address(*ip, *port);
    // load/entity_count/bounds filled by BSPTree::Deserialize after the
    // tree is rebuilt and PropagateBounds has run.
    return std::unique_ptr<BSPNode>{std::make_unique<BSPLeaf>(std::move(info))};
  }
  if (*tag == 1) {
    auto axis = r.Read<uint8_t>();
    auto pos = r.Read<float>();
    if (!axis || !pos) return Error{ErrorCode::kInvalidArgument, "BSPInternal: truncated"};
    if (*axis > 1) return Error{ErrorCode::kInvalidArgument, "BSPInternal: bad axis"};
    auto left = DeserializeNode(r);
    if (!left) return left.Error();
    auto right = DeserializeNode(r);
    if (!right) return right.Error();
    return std::unique_ptr<BSPNode>{std::make_unique<BSPInternal>(
        static_cast<BSPAxis>(*axis), *pos, std::move(*left), std::move(*right))};
  }
  return Error{ErrorCode::kInvalidArgument, "BSPNode: unknown tag"};
}

}  // namespace

void BSPTree::InitSingleCell(CellInfo info) {
  root_bounds_ = info.bounds.Area() > 0.f ? info.bounds : CellBounds{};
  primary_cell_id_ = info.cell_id;
  root_ = std::make_unique<BSPLeaf>(std::move(info));
  root_->PropagateBounds(root_bounds_);
}

namespace {

// Recursive descent used by Split - we walk through unique_ptr slots so
// the matching leaf can be replaced in-place by a fresh internal node.
auto SplitInSubtree(std::unique_ptr<BSPNode>& slot, const CellBounds& sub_bounds,
                    cellappmgr::CellID existing_cell_id, BSPAxis axis, float position,
                    CellInfo& new_cell) -> Result<void> {
  if (auto* leaf = dynamic_cast<BSPLeaf*>(slot.get())) {
    if (leaf->Info().cell_id != existing_cell_id) {
      return Error{ErrorCode::kNotFound, "BSPTree::Split: cell_id not found"};
    }
    // Bounds sanity: the split must land strictly inside the leaf's
    // current bounds on the requested axis.
    const float lo = (axis == BSPAxis::kX) ? leaf->Info().bounds.min_x : leaf->Info().bounds.min_z;
    const float hi = (axis == BSPAxis::kX) ? leaf->Info().bounds.max_x : leaf->Info().bounds.max_z;
    if (!(position > lo && position < hi)) {
      return Error{ErrorCode::kInvalidArgument,
                   "BSPTree::Split: position outside existing leaf bounds"};
    }
    auto left_leaf = std::move(slot);  // old leaf -> left side
    auto right_leaf = std::make_unique<BSPLeaf>(std::move(new_cell));
    slot =
        std::make_unique<BSPInternal>(axis, position, std::move(left_leaf), std::move(right_leaf));
    slot->PropagateBounds(sub_bounds);
    return {};
  }
  auto* internal = dynamic_cast<BSPInternal*>(slot.get());
  if (!internal) return Error{ErrorCode::kInternalError, "BSPTree::Split: unknown node type"};

  // Probe left-subtree leaves to find the target; fall through to right
  // otherwise. Tree height is O(log #cells) and entity routing goes
  // through FindCell not Split, so this linear probe is fine.
  std::vector<const CellInfo*> left_leaves;
  internal->Left()->CollectLeaves(left_leaves);
  bool in_left = false;
  for (const auto* ci : left_leaves) {
    if (ci->cell_id == existing_cell_id) {
      in_left = true;
      break;
    }
  }

  CellBounds left_b = sub_bounds;
  CellBounds right_b = sub_bounds;
  if (internal->Axis() == BSPAxis::kX) {
    left_b.max_x = internal->Position();
    right_b.min_x = internal->Position();
  } else {
    left_b.max_z = internal->Position();
    right_b.min_z = internal->Position();
  }
  return in_left ? SplitInSubtree(internal->LeftSlot(), left_b, existing_cell_id, axis, position,
                                  new_cell)
                 : SplitInSubtree(internal->RightSlot(), right_b, existing_cell_id, axis, position,
                                  new_cell);
}

}  // namespace

auto BSPTree::Split(cellappmgr::CellID existing_cell_id, BSPAxis axis, float position,
                    CellInfo new_cell) -> Result<void> {
  if (!root_) return Error{ErrorCode::kInvalidArgument, "BSPTree::Split: empty tree"};
  return SplitInSubtree(root_, root_bounds_, existing_cell_id, axis, position, new_cell);
}

namespace {

// Recursive descent for Unsplit — walks unique_ptr slots so the dying leaf's
// parent internal node can be replaced in-place by the sibling subtree.
auto UnsplitInSubtree(std::unique_ptr<BSPNode>& slot, const CellBounds& sub_bounds,
                      cellappmgr::CellID target_id) -> Result<void> {
  auto* internal = dynamic_cast<BSPInternal*>(slot.get());
  if (!internal) {
    // Recursion reached a leaf without finding the target's parent.
    return Error{ErrorCode::kNotFound, "BSPTree::Unsplit: cell_id not found"};
  }
  // Compute child sub-bounds for both descent + sibling promotion.
  CellBounds left_b = sub_bounds;
  CellBounds right_b = sub_bounds;
  if (internal->Axis() == BSPAxis::kX) {
    left_b.max_x = internal->Position();
    right_b.min_x = internal->Position();
  } else {
    left_b.max_z = internal->Position();
    right_b.min_z = internal->Position();
  }
  // Direct hit: one of our children is the dying leaf — promote the sibling.
  if (auto* left_leaf = dynamic_cast<BSPLeaf*>(internal->Left());
      left_leaf != nullptr && left_leaf->Info().cell_id == target_id) {
    auto sibling = std::move(internal->RightSlot());
    slot = std::move(sibling);
    slot->PropagateBounds(sub_bounds);
    return {};
  }
  if (auto* right_leaf = dynamic_cast<BSPLeaf*>(internal->Right());
      right_leaf != nullptr && right_leaf->Info().cell_id == target_id) {
    auto sibling = std::move(internal->LeftSlot());
    slot = std::move(sibling);
    slot->PropagateBounds(sub_bounds);
    return {};
  }
  // Otherwise probe each subtree for the target. Same linear scan Split uses.
  std::vector<const CellInfo*> left_leaves;
  internal->Left()->CollectLeaves(left_leaves);
  for (const auto* ci : left_leaves) {
    if (ci->cell_id == target_id)
      return UnsplitInSubtree(internal->LeftSlot(), left_b, target_id);
  }
  return UnsplitInSubtree(internal->RightSlot(), right_b, target_id);
}

}  // namespace

auto BSPTree::Unsplit(cellappmgr::CellID cell_id) -> Result<void> {
  if (!root_) return Error{ErrorCode::kInvalidArgument, "BSPTree::Unsplit: empty tree"};
  // Single-leaf tree: no sibling to promote, caller must handle as orphan.
  if (auto* leaf = dynamic_cast<BSPLeaf*>(root_.get());
      leaf != nullptr && leaf->Info().cell_id == cell_id) {
    return Error{ErrorCode::kInvalidArgument,
                 "BSPTree::Unsplit: cannot remove the only leaf"};
  }
  auto r = UnsplitInSubtree(root_, root_bounds_, cell_id);
  if (r.HasValue()) RecomputePrimaryCellId();
  return r;
}

auto BSPTree::FindCell(float x, float z) const -> const CellInfo* {
  return root_ ? root_->FindCell(x, z) : nullptr;
}

auto BSPTree::FindPrimaryCell(float x, float z, float ghost_region) const -> const CellInfo* {
  return root_ ? root_->FindPrimaryCell(x, z, ghost_region) : nullptr;
}

auto BSPTree::FindCellById(cellappmgr::CellID id) const -> const CellInfo* {
  if (!root_) return nullptr;
  std::vector<const CellInfo*> leaves;
  root_->CollectLeaves(leaves);
  for (const auto* ci : leaves) {
    if (ci->cell_id == id) return ci;
  }
  return nullptr;
}

auto BSPTree::FindCellByIdMutable(cellappmgr::CellID id) -> CellInfo* {
  if (!root_) return nullptr;
  std::vector<CellInfo*> leaves;
  root_->CollectLeaves(leaves);
  for (auto* ci : leaves) {
    if (ci->cell_id == id) return ci;
  }
  return nullptr;
}

void BSPTree::RecomputePrimaryCellId() {
  if (!root_) { primary_cell_id_ = 0; return; }
  std::vector<const CellInfo*> leaves;
  root_->CollectLeaves(leaves);
  primary_cell_id_ = leaves.empty() ? cellappmgr::CellID{0} : leaves.front()->cell_id;
}

void BSPTree::VisitRect(const CellBounds& rect,
                        const std::function<void(const CellInfo&)>& visitor) const {
  if (root_) root_->VisitRect(rect, visitor);
}

void BSPTree::UpdateLoad() {
  if (root_) root_->UpdateLoad();
}

void BSPTree::Balance(float safety_bound) {
  if (!root_) return;
  root_->UpdateLoad();
  root_->Balance(safety_bound);
}

auto BSPTree::Leaves() const -> std::vector<const CellInfo*> {
  std::vector<const CellInfo*> out;
  if (root_) root_->CollectLeaves(out);
  return out;
}

auto BSPTree::LeavesMutable() -> std::vector<CellInfo*> {
  std::vector<CellInfo*> out;
  if (root_) root_->CollectLeaves(out);
  return out;
}

void BSPTree::Serialize(BinaryWriter& w) const {
  root_bounds_.Serialize(w);
  const uint8_t has_root = root_ ? 1 : 0;
  w.Write(has_root);
  if (root_) root_->Serialize(w);
}

auto BSPTree::Deserialize(BinaryReader& r) -> Result<BSPTree> {
  auto root_bounds = CellBounds::Deserialize(r);
  if (!root_bounds) return root_bounds.Error();
  auto has_root = r.Read<uint8_t>();
  if (!has_root) return Error{ErrorCode::kInvalidArgument, "BSPTree: truncated"};
  BSPTree t;
  t.root_bounds_ = *root_bounds;
  if (*has_root != 0) {
    auto node = DeserializeNode(r);
    if (!node) return node.Error();
    t.root_ = std::move(*node);
    t.root_->PropagateBounds(t.root_bounds_);
  }
  t.RecomputePrimaryCellId();
  return t;
}

}  // namespace atlas
