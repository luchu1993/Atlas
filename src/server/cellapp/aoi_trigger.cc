#include "aoi_trigger.h"

#include "cell_entity.h"
#include "space/entity_range_list_node.h"
#include "witness.h"

namespace atlas {

AoITrigger::AoITrigger(Witness& witness, RangeListNode& central, float range)
    : RangeTrigger(central, range), witness_(witness) {}

namespace {

// Recover the CellEntity* a RangeListNode belongs to. Returns nullptr
// when the crosser is not an entity node (e.g. another trigger's bound,
// or the sentinel head/tail).
auto OwnerOf(RangeListNode& node) -> CellEntity* {
  // Only EntityRangeListNode instances carry an owner back-pointer.
  // Non-entity nodes (trigger bounds, sentinels) report Order() other
  // than kEntity, so filter quickly on that first.
  if (node.Order() != RangeListOrder::kEntity) return nullptr;
  auto& entity_node = static_cast<EntityRangeListNode&>(node);
  return static_cast<CellEntity*>(entity_node.OwnerData());
}

}  // namespace

void AoITrigger::OnEnter(RangeListNode& other) {
  if (auto* peer = OwnerOf(other)) witness_.HandleAoIEnter(*peer);
}

void AoITrigger::OnLeave(RangeListNode& other) {
  if (auto* peer = OwnerOf(other)) witness_.HandleAoILeave(*peer);
}

}  // namespace atlas
