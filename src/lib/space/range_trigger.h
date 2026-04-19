#ifndef ATLAS_LIB_SPACE_RANGE_TRIGGER_H_
#define ATLAS_LIB_SPACE_RANGE_TRIGGER_H_

#include <unordered_set>

#include "space/range_list.h"
#include "space/range_trigger_node.h"

namespace atlas {

// ============================================================================
// RangeTrigger — axis-aligned square 2-D region attached to a central node
//
// A RangeTrigger fires OnEnter / OnLeave callbacks as other RangeListNodes
// cross the square [(cx-r, cz-r), (cx+r, cz+r)] centred on its `central_`
// node. The implementation decomposes the 2-D membership test into two
// linked-list shuffles plus a cheap orthogonal-axis bounds check, reusing
// BigWorld's 15-year-old algorithm.
//
// The trigger plants two sentinel-like nodes into the owning RangeList:
//   - lower_bound_ at (cx - r, cz - r)
//   - upper_bound_ at (cx + r, cz + r)
// When any peer crosses either bound, the cross callback checks the peer's
// orthogonal coordinate (fed in via `other_ortho`) against the trigger's
// extent on that axis. Only a crossing that both changes the one-axis
// membership AND has the orthogonal inside the range flips the 2-D state —
// producing an enter or leave event.
//
// Moving the centre (after the owner entity moves) is driven by calling
// Update(old_x, old_z); the trigger shuffles both bounds to their new list
// position, firing the enter/leave events accumulated during the shuffle.
// SetRange(new_range) reshapes the trigger in place with the same mechanic.
//
// Ordering trick ("expand first, contract second"):
//   When central moves on the X axis by +dx, the upper bound moves +dx
//   (right, expanding the right side) while the lower bound also moves +dx
//   (right, contracting the left side). Processing the expanding bound
//   FIRST means any peer that sits near the moving-side boundary sees the
//   trigger grow around it before the opposite side contracts past it, so
//   transient "leave then immediately re-enter" glitches are avoided.
//   The current implementation does the per-axis ordering on X only and
//   folds Z into the same pass; for Phase 10's single-CellApp workloads
//   this is enough. Mixed-direction moves remain correct but may fire a
//   leave/enter pair at exactly-on-boundary peers (documented).
// ============================================================================

class RangeTrigger {
 public:
  RangeTrigger(RangeListNode& central, float range);
  virtual ~RangeTrigger();

  RangeTrigger(const RangeTrigger&) = delete;
  auto operator=(const RangeTrigger&) -> RangeTrigger& = delete;

  // Link the trigger's bound nodes into `list`. The caller also needs to
  // ensure the `central_` node is already (or will be) in the same list,
  // but the trigger doesn't manage central's linkage itself.
  void Insert(RangeList& list);

  // Unlink the bound nodes. Fires no OnLeave events — callers that need
  // "tell me about peers still inside at teardown time" must walk the
  // list themselves. (BigWorld callers almost always Remove during an
  // entity-destroy flow where the trigger's observer is about to vanish,
  // so spurious leaves would be noise.)
  void Remove(RangeList& list);

  // Centre moved: bounds' computed (x, z) now disagree with their list
  // position. Re-shuffle them — this is where OnEnter / OnLeave fire for
  // peers whose 2-D membership just flipped.
  void Update(float old_central_x, float old_central_z);

  // Change range in-place without the centre moving. Expansion comes
  // first so that new-coverage events fire cleanly before any contraction
  // triggers leaves.
  void SetRange(float new_range);

  [[nodiscard]] auto Central() const -> const RangeListNode& { return central_; }
  [[nodiscard]] auto Range() const -> float { return range_; }

  // Callbacks — subclass (AoITrigger, ProximityTrigger) overrides. The
  // `other` node is guaranteed to be non-bound (a peer in the list) and
  // not the central itself.
  virtual void OnEnter(RangeListNode& other) = 0;
  virtual void OnLeave(RangeListNode& other) = 0;

 private:
  friend class RangeTriggerNode;

  // Invoked by RangeTriggerNode when the bound crosses a peer on one
  // axis. Consumers fold the orthogonal-axis coord to decide if the
  // single-axis crossing is also a 2-D transition.
  void HandleCrossX(RangeTriggerNode& bound, RangeListNode& other, bool positive,
                    float other_z_at_cross);
  void HandleCrossZ(RangeTriggerNode& bound, RangeListNode& other, bool positive,
                    float other_x_at_cross);

  // Commit a derived 2-D membership decision for `peer`: fire OnEnter /
  // OnLeave only on the edge transition (uses inside_peers_ as the
  // authoritative state).
  void DispatchMembership(RangeListNode* peer, bool now_inside);

  // Is `other`'s Z-coord inside the trigger's Z extent?
  [[nodiscard]] auto IsZInRange(float z) const -> bool;
  [[nodiscard]] auto IsXInRange(float x) const -> bool;

  RangeListNode& central_;
  float range_;

  // The bound nodes embed backrefs so their crossed callbacks can locate
  // this trigger. They MUST be initialised after `central_` and `range_`
  // so RangeTriggerNode::X()/Z() read valid state during Insert.
  RangeTriggerNode lower_bound_;
  RangeTriggerNode upper_bound_;

  RangeList* list_{nullptr};

  // Peers currently 2-D inside. A single 2-D transition can generate
  // multiple single-axis crosses (inserting a new peer bubbles past
  // lower_x AND lower_z; moving diagonally across the box crosses
  // both upper_x AND upper_z), so the handler uses this set to suppress
  // duplicate OnEnter/OnLeave callbacks — only true state transitions
  // fire events.
  std::unordered_set<RangeListNode*> inside_peers_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_RANGE_TRIGGER_H_
