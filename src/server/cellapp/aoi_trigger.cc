#include "aoi_trigger.h"

#include <cassert>

#include "cell_entity.h"
#include "space/entity_range_list_node.h"
#include "space/range_list.h"
#include "witness.h"

namespace atlas {

namespace {

// Recover the CellEntity a RangeListNode belongs to. Returns nullptr
// when the crosser is not an entity node (sentinel head/tail, another
// trigger's bound). Only EntityRangeListNode carries an owner back-pointer.
auto OwnerOf(RangeListNode& node) -> CellEntity* {
  if (node.Order() != RangeListOrder::kEntity) return nullptr;
  auto& entity_node = static_cast<EntityRangeListNode&>(node);
  return static_cast<CellEntity*>(entity_node.OwnerData());
}

}  // namespace

// Inner-band: fires OnEnter into the witness on inbound crossings. Leaves
// between inner and outer are swallowed to produce hysteresis — the peer
// stays in AoI until outer's OnLeave fires.
class AoITrigger::InnerTrigger final : public RangeTrigger {
 public:
  InnerTrigger(Witness& w, RangeListNode& central, float range)
      : RangeTrigger(central, range), witness_(w) {}

  void OnEnter(RangeListNode& other) override {
    if (auto* peer = OwnerOf(other)) witness_.HandleAoIEnter(*peer);
  }

  void OnLeave(RangeListNode& /*other*/) override {
    // Suppressed by design — see class comment in aoi_trigger.h.
  }

 private:
  Witness& witness_;
};

// Outer-band: fires OnLeave into the witness on outbound crossings. Enters
// from far outside into the hysteresis window are swallowed — only inner's
// OnEnter promotes a peer into AoI.
class AoITrigger::OuterTrigger final : public RangeTrigger {
 public:
  OuterTrigger(Witness& w, RangeListNode& central, float range)
      : RangeTrigger(central, range), witness_(w) {}

  void OnEnter(RangeListNode& /*other*/) override {
    // Suppressed by design — see class comment in aoi_trigger.h.
  }

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
  // Order: inner before outer. With two triggers on the same central,
  // peers already 2-D inside at insertion time fire OnEnter on both
  // during Insert's natural sweep. Inner first means the witness sees
  // the AoI enter before outer's (suppressed) OnEnter fires — cleaner
  // trace logs. Correctness is independent of order.
  inner_->Insert(list);
  outer_->Insert(list);
}

void AoITrigger::Remove(RangeList& list) {
  // Reverse insertion order out of habit — neither trigger fires OnLeave
  // during Remove, so the order is cosmetic.
  outer_->Remove(list);
  inner_->Remove(list);
}

void AoITrigger::SetBounds(float inner_range, float outer_range) {
  assert(inner_range >= 0.f);
  assert(outer_range >= inner_range);
  // "Expand first, contract second" applied across both bands. When
  // expanding we grow outer first so a peer newly inside outer lands in
  // the hysteresis window before inner reaches them; when contracting
  // we shrink inner first so the suppressed inner-leaves run ahead of
  // outer's (visible) leaves. Correctness is actually order-independent
  // because inner's OnLeave and outer's OnEnter are both suppressed,
  // but the ordering keeps user-visible trace sequences tidy.
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
