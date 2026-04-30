#ifndef ATLAS_LIB_SPACE_RANGE_TRIGGER_NODE_H_
#define ATLAS_LIB_SPACE_RANGE_TRIGGER_NODE_H_

#include "space/range_list_node.h"

namespace atlas {

class RangeTrigger;

class RangeTriggerNode final : public RangeListNode {
 public:
  RangeTriggerNode(RangeTrigger& owner, bool is_upper);

  [[nodiscard]] auto X() const -> float override;
  [[nodiscard]] auto Z() const -> float override;
  [[nodiscard]] auto Order() const -> RangeListOrder override;

  [[nodiscard]] auto Owner() -> RangeTrigger& { return owner_; }
  [[nodiscard]] auto IsUpper() const -> bool { return is_upper_; }

 protected:
  void OnCrossedX(RangeListNode& other, bool positive, float other_ortho) override;
  void OnCrossedZ(RangeListNode& other, bool positive, float other_ortho) override;

 private:
  RangeTrigger& owner_;
  bool is_upper_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_RANGE_TRIGGER_NODE_H_
