#include "space/proximity_controller.h"

#include <utility>

#include "space/range_list.h"

namespace atlas {

// The private TriggerImpl defers to the caller-supplied lambdas. Keeping
// it PIMPL-style hides RangeTrigger's full definition from the header's
// public surface — consumers of ProximityController don't need to know how
// the bounds are stored.
class ProximityController::TriggerImpl final : public RangeTrigger {
 public:
  TriggerImpl(ProximityController& owner, RangeListNode& central, float range)
      : RangeTrigger(central, range), owner_(owner) {}

  void OnEnter(RangeListNode& other) override {
    if (owner_.on_enter_) owner_.on_enter_(owner_, other);
  }
  void OnLeave(RangeListNode& other) override {
    if (owner_.on_leave_) owner_.on_leave_(owner_, other);
  }

 private:
  ProximityController& owner_;
};

ProximityController::ProximityController(RangeListNode& central, RangeList& list, float range,
                                         EnterFn on_enter, LeaveFn on_leave)
    : central_(central),
      list_(list),
      range_(range),
      on_enter_(std::move(on_enter)),
      on_leave_(std::move(on_leave)) {}

ProximityController::~ProximityController() = default;

void ProximityController::Start() {
  // Trigger is constructed here (rather than in the ctor) so no RangeList
  // insertion happens until the Controllers container has finished
  // registering this instance — matches the lifecycle of the other
  // concrete controllers.
  trigger_ = std::make_unique<TriggerImpl>(*this, central_, range_);
  trigger_->Insert(list_);
}

void ProximityController::Update(float /*dt*/) {
  // Purely event-driven; the RangeList shuffle drives all state changes.
}

void ProximityController::Stop() {
  if (trigger_) {
    trigger_->Remove(list_);
    trigger_.reset();
  }
}

void ProximityController::SetRange(float new_range) {
  range_ = new_range;
  if (trigger_) trigger_->SetRange(new_range);
}

}  // namespace atlas
