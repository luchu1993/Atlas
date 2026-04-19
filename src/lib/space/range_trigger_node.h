#ifndef ATLAS_LIB_SPACE_RANGE_TRIGGER_NODE_H_
#define ATLAS_LIB_SPACE_RANGE_TRIGGER_NODE_H_

#include "space/range_list_node.h"

namespace atlas {

class RangeTrigger;

// ============================================================================
// RangeTriggerNode — bound node (lower or upper) owned by a RangeTrigger
//
// The node's x/z are derived on demand from `trigger.central().{X,Z}()` plus
// the signed `trigger.range_`. When the central entity moves, the bound's
// reported coordinates change immediately; a subsequent RangeTrigger::Update()
// drives the RangeList shuffle that realigns the bound's place in the list.
//
// Cross callbacks forward to the owning RangeTrigger, which reconstructs the
// 2-D enter/leave decision using the `other_ortho` argument and the trigger's
// known extent on the orthogonal axis.
// ============================================================================

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
