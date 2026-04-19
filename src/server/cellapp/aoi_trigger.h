#ifndef ATLAS_SERVER_CELLAPP_AOI_TRIGGER_H_
#define ATLAS_SERVER_CELLAPP_AOI_TRIGGER_H_

#include "space/range_trigger.h"

namespace atlas {

class CellEntity;
class Witness;

// ============================================================================
// AoITrigger — RangeTrigger specialisation that fans out to a Witness
//
// The trigger's central node is the observer's EntityRangeListNode. When
// any peer entity's node crosses the bound, AoITrigger identifies the
// CellEntity that owns the crossing node and calls back into the witness.
//
// A trigger's central is ALWAYS an EntityRangeListNode (not a generic
// RangeListNode), so we can recover the owning CellEntity through a
// narrow backchannel the witness plumbs in (via SetOwnerLookup). Keeping
// the lookup as a function pointer rather than dynamic_cast avoids
// leaking CellEntity's layout into the space library.
// ============================================================================

class AoITrigger final : public RangeTrigger {
 public:
  AoITrigger(Witness& witness, RangeListNode& central, float range);

  void OnEnter(RangeListNode& other) override;
  void OnLeave(RangeListNode& other) override;

 private:
  Witness& witness_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_AOI_TRIGGER_H_
