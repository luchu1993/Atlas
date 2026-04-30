#ifndef ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_
#define ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_

#include "space/range_list_node.h"

namespace atlas {

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

  // Opaque back-reference; the setter and caster must agree on the concrete type.
  void SetOwnerData(void* owner) { owner_data_ = owner; }
  [[nodiscard]] auto OwnerData() const -> void* { return owner_data_; }

 private:
  float x_;
  float z_;
  void* owner_data_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_ENTITY_RANGE_LIST_NODE_H_
