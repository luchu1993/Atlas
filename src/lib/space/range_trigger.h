#ifndef ATLAS_LIB_SPACE_RANGE_TRIGGER_H_
#define ATLAS_LIB_SPACE_RANGE_TRIGGER_H_

#include <unordered_set>

#include "space/range_list.h"
#include "space/range_trigger_node.h"

namespace atlas {

class RangeTrigger {
 public:
  RangeTrigger(RangeListNode& central, float range);
  virtual ~RangeTrigger();

  RangeTrigger(const RangeTrigger&) = delete;
  auto operator=(const RangeTrigger&) -> RangeTrigger& = delete;

  // The caller owns central_ linkage; RangeTrigger only manages its bounds.
  void Insert(RangeList& list);

  // Teardown does not synthesize OnLeave events.
  void Remove(RangeList& list);

  void Update(float old_central_x, float old_central_z);

  void SetRange(float new_range);

  [[nodiscard]] auto Central() const -> const RangeListNode& { return central_; }
  [[nodiscard]] auto Range() const -> float { return range_; }

  // Migration seeds must be applied before Insert(), while bounds are unlinked.
  [[nodiscard]] auto InsidePeers() const -> const std::unordered_set<RangeListNode*>& {
    return inside_peers_;
  }
  void SeedInsidePeersForMigration(std::unordered_set<RangeListNode*> peers);

  // Keeps nested trigger bands consistent when cross-event ordering lags.
  bool ForceInsidePeer(RangeListNode* peer) { return inside_peers_.insert(peer).second; }

  virtual void OnEnter(RangeListNode& other) = 0;
  virtual void OnLeave(RangeListNode& other) = 0;

 private:
  friend class RangeTriggerNode;

  void HandleCrossX(RangeTriggerNode& bound, RangeListNode& other, bool positive,
                    float other_z_at_cross);
  void HandleCrossZ(RangeTriggerNode& bound, RangeListNode& other, bool positive,
                    float other_x_at_cross);

  void DispatchMembership(RangeListNode* peer, bool now_inside);

  [[nodiscard]] auto IsZInRange(float z) const -> bool;
  [[nodiscard]] auto IsXInRange(float x) const -> bool;

  RangeListNode& central_;
  float range_;

  RangeTriggerNode lower_bound_;
  RangeTriggerNode upper_bound_;

  RangeList* list_{nullptr};

  // Authoritative 2-D membership; suppresses duplicate single-axis crosses.
  std::unordered_set<RangeListNode*> inside_peers_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_RANGE_TRIGGER_H_
