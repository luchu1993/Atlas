#ifndef ATLAS_LIB_SPACE_RANGE_LIST_H_
#define ATLAS_LIB_SPACE_RANGE_LIST_H_

#include <cfloat>

#include "space/range_list_node.h"

namespace atlas {

class RangeList {
 public:
  RangeList();
  ~RangeList() = default;

  RangeList(const RangeList&) = delete;
  auto operator=(const RangeList&) -> RangeList& = delete;

  void Insert(RangeListNode* node);

  void Remove(RangeListNode* node);

  // Re-sort `node` after its coordinates changed. `old_x` / `old_z` are
  // previous coordinates used by 2-D cross-notification.
  void ShuffleXThenZ(RangeListNode* node, float old_x, float old_z);

  [[nodiscard]] auto Head() -> RangeListNode& { return head_; }
  [[nodiscard]] auto Tail() -> RangeListNode& { return tail_; }

 private:
  void ShuffleX(RangeListNode* node, float old_z_of_node);
  void ShuffleZ(RangeListNode* node, float old_x_of_node);

  FixedRangeListNode head_{-FLT_MAX, -FLT_MAX, RangeListOrder::kHead};
  FixedRangeListNode tail_{+FLT_MAX, +FLT_MAX, RangeListOrder::kTail};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_RANGE_LIST_H_
