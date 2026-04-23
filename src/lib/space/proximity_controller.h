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

// ============================================================================
// ProximityController — event-driven "things inside a radius" trigger
//
// Wraps RangeTrigger and exposes it as a Controller so script-side code can
// attach/detach proximity sensors through the same management surface used
// for movement and timers. Unlike MoveToPointController, ProximityController
// is ENTIRELY event-driven: Update(dt) is a no-op. All work happens in the
// OnEnter / OnLeave callbacks the RangeList shuffle drives.
//
// Construction needs the owning entity's RangeListNode (the "central" of
// the trigger) and a RangeList to plant the bound nodes into. Ownership of
// the trigger rests with the controller; Stop() tears it down.
// ============================================================================

class ProximityController final : public Controller {
 public:
  using EnterFn = std::function<void(ProximityController& self, RangeListNode& other)>;
  using LeaveFn = std::function<void(ProximityController& self, RangeListNode& other)>;

  ProximityController(RangeListNode& central, RangeList& list, float range, EnterFn on_enter,
                      LeaveFn on_leave);
  // Out-of-line destructor so the TriggerImpl (PIMPL) definition is in
  // scope at deletion time; inlining would force every translation unit
  // that includes this header to instantiate ~TriggerImpl.
  ~ProximityController() override;

  [[nodiscard]] auto Range() const -> float { return range_; }

  // Re-target the proximity radius in place. Fires enter/leave events for
  // peers that were near the boundary.
  void SetRange(float new_range);

  // Controller hooks.
  void Start() override;
  void Update(float dt) override;
  void Stop() override;
  [[nodiscard]] auto TypeTag() const -> ControllerKind override {
    return ControllerKind::kProximity;
  }

  // Snapshot of peers currently inside the trigger. Empty before Start().
  // Offload serialisation consumes this to remember peer membership across
  // process migration; see controller_codec.h.
  [[nodiscard]] auto InsidePeers() const -> const std::unordered_set<RangeListNode*>&;

  // Seed the inside-peer set before Start() runs. Used on Offload arrival:
  // pre-populating with peers that were already inside at the origin
  // suppresses duplicate OnEnter events for peers that remained in range
  // across migration. Peers from the origin that no longer exist locally
  // must be dropped by the caller first — the trigger won't know about
  // them and will not emit phantom OnLeave events.
  void SeedInsidePeersForMigration(std::unordered_set<RangeListNode*> peers);

 private:
  class TriggerImpl;

  // Applied in Start() when non-empty. Owned by this controller until
  // migrated into the TriggerImpl's own inside_peers_ set.
  std::unordered_set<RangeListNode*> pending_seed_peers_;

  RangeListNode& central_;
  RangeList& list_;
  float range_;
  EnterFn on_enter_;
  LeaveFn on_leave_;

  // Owned here so the trigger's lifetime matches Start/Stop rather than
  // construction/destruction. Tests hold the controller through a
  // unique_ptr<Controller> so leaving the trigger uninitialised until
  // Start() would be surprising, but it lets Controllers::Add's Start
  // semantics stay consistent.
  std::unique_ptr<TriggerImpl> trigger_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_PROXIMITY_CONTROLLER_H_
