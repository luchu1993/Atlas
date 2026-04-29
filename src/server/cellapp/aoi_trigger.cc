#include "aoi_trigger.h"

#include <cassert>

#include "cell_entity.h"
#include "space/entity_range_list_node.h"
#include "space/range_list.h"
#include "witness.h"

namespace atlas {

namespace {

// Returns nullptr for non-entity nodes (sentinels, trigger bounds).
auto OwnerOf(RangeListNode& node) -> CellEntity* {
  if (node.Order() != RangeListOrder::kEntity) return nullptr;
  auto& entity_node = static_cast<EntityRangeListNode&>(node);
  return static_cast<CellEntity*>(entity_node.OwnerData());
}

}  // namespace

// Inner band: fires OnEnter; OnLeave suppressed (hysteresis).
class AoITrigger::InnerTrigger final : public RangeTrigger {
 public:
  InnerTrigger(Witness& w, RangeListNode& central, float range)
      : RangeTrigger(central, range), witness_(w) {}

  void OnEnter(RangeListNode& other) override {
    if (auto* peer = OwnerOf(other)) {
      witness_.HandleAoIEnter(*peer);
      // Inner ⊂ outer invariant: a fast-path inner-enter (observer move
      // shuffles inner before outer, hysteresis re-cross) leaves outer
      // stale; force-insert so the eventual outbound cross fires Leave.
      witness_.ForceOuterInsidePeer(other);
    }
  }

  void OnLeave(RangeListNode& /*other*/) override {}  // hysteresis

 private:
  Witness& witness_;
};

// Outer band: fires OnLeave; OnEnter suppressed (hysteresis-only).
class AoITrigger::OuterTrigger final : public RangeTrigger {
 public:
  OuterTrigger(Witness& w, RangeListNode& central, float range)
      : RangeTrigger(central, range), witness_(w) {}

  void OnEnter(RangeListNode& /*other*/) override {}  // hysteresis

  void OnLeave(RangeListNode& other) override {
    if (auto* peer = OwnerOf(other)) witness_.HandleAoILeave(*peer);
  }

 private:
  Witness& witness_;
};

AoITrigger::AoITrigger(Witness& witness, EntityRangeListNode& central, float inner_range,
                       float outer_range)
    : inner_(std::make_unique<InnerTrigger>(witness, central, inner_range)),
      outer_(std::make_unique<OuterTrigger>(witness, central, outer_range)) {
  assert(inner_range >= 0.f);
  assert(outer_range >= inner_range && "outer_range must include hysteresis band");
}

AoITrigger::~AoITrigger() = default;

void AoITrigger::Insert(RangeList& list) {
  // Inner first for cleaner trace order; correctness is order-independent.
  inner_->Insert(list);
  outer_->Insert(list);
}

void AoITrigger::Remove(RangeList& list) {
  outer_->Remove(list);
  inner_->Remove(list);
}

void AoITrigger::ForceOuterInsidePeer(RangeListNode& peer) {
  outer_->ForceInsidePeer(&peer);
}

void AoITrigger::OnCentralMoved(float old_central_x, float old_central_z) {
  inner_->Update(old_central_x, old_central_z);
  outer_->Update(old_central_x, old_central_z);
}

void AoITrigger::SetBounds(float inner_range, float outer_range) {
  assert(inner_range >= 0.f);
  assert(outer_range >= inner_range);
  // Expand outer first / contract inner first keeps trace order tidy;
  // suppressed leaves on inner and enters on outer make this cosmetic.
  const bool expanding = outer_range > outer_->Range();
  if (expanding) {
    outer_->SetRange(outer_range);
    inner_->SetRange(inner_range);
  } else {
    inner_->SetRange(inner_range);
    outer_->SetRange(outer_range);
  }
}

auto AoITrigger::InnerRange() const -> float {
  return inner_->Range();
}

auto AoITrigger::OuterRange() const -> float {
  return outer_->Range();
}

}  // namespace atlas
