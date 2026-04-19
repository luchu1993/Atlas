#include "space/proximity_controller.h"

#include <cassert>
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
  // Phase 11 PR-6 review-fix B2: on cross-process Offload arrival the
  // caller pre-populates pending_seed_peers_ with peers that were
  // "inside" at the origin CellApp. Seed them before Insert so the
  // natural enter/leave dispatch only fires leave events for peers
  // that left and suppresses duplicate enter events for peers that
  // stayed.
  if (!pending_seed_peers_.empty()) {
    trigger_->SeedInsidePeersForMigration(std::move(pending_seed_peers_));
  }
  trigger_->Insert(list_);
}

auto ProximityController::InsidePeers() const -> const std::unordered_set<RangeListNode*>& {
  static const std::unordered_set<RangeListNode*> kEmpty;
  return trigger_ ? trigger_->InsidePeers() : kEmpty;
}

void ProximityController::SeedInsidePeersForMigration(std::unordered_set<RangeListNode*> peers) {
  // Must be called between ctor and Controllers::Add (which calls Start).
  // If called after Start, the trigger is already inserted and the seed
  // would be useless — this assertion catches the misuse.
  assert(trigger_ == nullptr && "SeedInsidePeersForMigration after Start()");
  pending_seed_peers_ = std::move(peers);
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
