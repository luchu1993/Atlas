#ifndef ATLAS_LIB_SPACE_PROXIMITY_CONTROLLER_H_
#define ATLAS_LIB_SPACE_PROXIMITY_CONTROLLER_H_

#include <functional>
#include <memory>
#include <unordered_set>

#include "space/controller.h"
#include "space/range_trigger.h"

namespace atlas {

class RangeList;
class RangeListNode;

class ProximityController final : public Controller {
 public:
  using EnterFn = std::function<void(ProximityController& self, RangeListNode& other)>;
  using LeaveFn = std::function<void(ProximityController& self, RangeListNode& other)>;

  ProximityController(RangeListNode& central, RangeList& list, float range, EnterFn on_enter,
                      LeaveFn on_leave);
  ~ProximityController() override;

  [[nodiscard]] auto Range() const -> float { return range_; }

  void SetRange(float new_range);

  void Start() override;
  void Update(float dt) override;
  void Stop() override;
  [[nodiscard]] auto TypeTag() const -> ControllerKind override {
    return ControllerKind::kProximity;
  }

  [[nodiscard]] auto InsidePeers() const -> const std::unordered_set<RangeListNode*>&;

  // Seed before Start() so migration restore does not replay duplicate enters.
  void SeedInsidePeersForMigration(std::unordered_set<RangeListNode*> peers);

 private:
  class TriggerImpl;

  std::unordered_set<RangeListNode*> pending_seed_peers_;

  RangeListNode& central_;
  RangeList& list_;
  float range_;
  EnterFn on_enter_;
  LeaveFn on_leave_;

  std::unique_ptr<TriggerImpl> trigger_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_PROXIMITY_CONTROLLER_H_
