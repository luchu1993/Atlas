#ifndef ATLAS_LIB_SPACE_RANGE_LIST_H_
#define ATLAS_LIB_SPACE_RANGE_LIST_H_

#include <cfloat>

#include "space/range_list_node.h"

namespace atlas {

// ============================================================================
// RangeList — doubly-indexed sorted list on (x, z)
//
// Nodes are linked into two independent doubly-linked lists, one sorted by
// X and one sorted by Z. Each list is bounded by fixed head/tail sentinels
// so the shuffle loops never need to null-check their neighbours.
//
// All operations are O(δ) where δ is the displacement in rank, not in
// coordinate space. For bounded-speed entities in a dense region this is
// effectively O(1) per tick; the worst case only degrades when a node
// teleports across the whole world.
//
// Thread model: single-threaded. Callers must not touch the list from
// multiple threads concurrently; the tick loop is expected to drive it.
// ============================================================================

class RangeList {
 public:
  RangeList();
  ~RangeList() = default;

  RangeList(const RangeList&) = delete;
  auto operator=(const RangeList&) -> RangeList& = delete;

  // Link `node` into both axes using its current X()/Z() position. The
  // node must not already be linked; double-insertion is a programmer
  // error. Insertion is amortised O(δ) where δ is the rank distance from
  // head to node's final position (so worst-case O(N), expected small in
  // practice because entities spawn near other entities).
  void Insert(RangeListNode* node);

  // Unlink `node` from both axes. The node's prev/next pointers are
  // zeroed so accidental reuse trips an assert rather than corrupting
  // the list silently.
  void Remove(RangeListNode* node);

  // Re-sort `node` after its coordinates changed. `old_x` / `old_z` are
  // the previous coordinates — the 2-D cross-notification logic needs
  // them to decide whether a crossing is inside or outside the peer's
  // orthogonal bounds. Calling this after every movement (even a zero
  // move) is safe; the shuffle loops exit on the first round where no
  // swap occurs.
  void ShuffleXThenZ(RangeListNode* node, float old_x, float old_z);

  // Sentinels exposed for tests — sometimes it's useful to walk from
  // head to tail to validate invariants after a random workload.
  [[nodiscard]] auto Head() -> RangeListNode& { return head_; }
  [[nodiscard]] auto Tail() -> RangeListNode& { return tail_; }

 private:
  // X-axis and Z-axis pass: bubble `node` left then right on the given
  // axis until every neighbour is strictly less-or-equal / greater-or-
  // equal respectively. Calls OnCrossedX / OnCrossedZ on every swap
  // where the two nodes have matching flags.
  void ShuffleX(RangeListNode* node, float old_z_of_node);
  void ShuffleZ(RangeListNode* node, float old_x_of_node);

  // The two sentinels double as owners of the "empty list" state, so a
  // RangeList is valid immediately after construction with no entities.
  FixedRangeListNode head_{-FLT_MAX, -FLT_MAX, RangeListOrder::kHead};
  FixedRangeListNode tail_{+FLT_MAX, +FLT_MAX, RangeListOrder::kTail};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_RANGE_LIST_H_
