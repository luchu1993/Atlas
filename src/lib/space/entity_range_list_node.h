#ifndef ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_
#define ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_

#include "space/range_list_node.h"

namespace atlas {

// ============================================================================
// EntityRangeListNode — the RangeListNode attached to every CellEntity
//
// Entities are mutable points in world space. Their X/Z mirror the owning
// CellEntity's position and are updated before the RangeList shuffle runs.
// This class lives in src/lib/space (not src/server/cellapp) so tests and
// future non-entity clients of RangeList can reuse the "mutable coord +
// opt-in to trigger flags" shape without dragging in the server layer.
//
// Flag vocabulary:
//   wants_flags_ = kLowerAoiTrigger | kUpperAoiTrigger | kEntityTrigger
//     — the node receives cross callbacks from AoI trigger bounds (so it
//     could, e.g., update an "I'm inside X trigger" counter) AND from
//     other entity nodes (used by ProximityController). It's declared as
//     opting in on reception; the actual callbacks are no-ops unless a
//     subclass overrides.
//   makes_flags_ = kIsEntity | kEntityTrigger
//     — peers that want entity crossings (the bound of any trigger) see
//     our motion events. kIsEntity lets trigger bounds distinguish peers
//     from sentinels or from other triggers.
// ============================================================================

class EntityRangeListNode : public RangeListNode {
 public:
  EntityRangeListNode(float x, float z) : x_(x), z_(z) {
    makes_flags_ = RangeListFlags::kIsEntity | RangeListFlags::kEntityTrigger;
    wants_flags_ = RangeListFlags::kLowerAoiTrigger | RangeListFlags::kUpperAoiTrigger |
                   RangeListFlags::kEntityTrigger;
  }

  [[nodiscard]] auto X() const -> float override { return x_; }
  [[nodiscard]] auto Z() const -> float override { return z_; }
  [[nodiscard]] auto Order() const -> RangeListOrder override { return RangeListOrder::kEntity; }

  // Called by CellEntity AFTER it has linked this node into a RangeList
  // and BEFORE a follow-up ShuffleXThenZ that propagates the new coords.
  // Direct field mutation keeps the hot path branch-free.
  void SetXZ(float x, float z) {
    x_ = x;
    z_ = z;
  }

  // Opaque back-reference to the owning object (CellEntity in practice).
  // The space library deliberately doesn't know CellEntity's layout —
  // consumers use this slot to recover their domain object from a raw
  // RangeListNode* handed out by RangeTrigger callbacks. reinterpret_cast
  // back to the owning type is the intended pattern; the contract is
  // that whoever SetOwnerData'd the pointer must also be the one who
  // casts it.
  void SetOwnerData(void* owner) { owner_data_ = owner; }
  [[nodiscard]] auto OwnerData() const -> void* { return owner_data_; }

 private:
  float x_;
  float z_;
  void* owner_data_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_
