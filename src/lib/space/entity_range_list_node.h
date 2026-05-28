#ifndef ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_
#define ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_

#include "space/range_list_node.h"

namespace atlas {

class CellEntity;

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

  void SetXZ(float x, float z) {
    x_ = x;
    z_ = z;
  }

  void SetOwner(CellEntity* owner) { owner_ = owner; }
  [[nodiscard]] auto Owner() const -> CellEntity* { return owner_; }

 private:
  float x_;
  float z_;
  CellEntity* owner_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_
