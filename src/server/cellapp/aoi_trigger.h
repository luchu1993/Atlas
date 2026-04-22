#ifndef ATLAS_SERVER_CELLAPP_AOI_TRIGGER_H_
#define ATLAS_SERVER_CELLAPP_AOI_TRIGGER_H_

#include <memory>

#include "space/range_trigger.h"

namespace atlas {

class CellEntity;
class EntityRangeListNode;
class RangeList;
class Witness;

// ============================================================================
// AoITrigger — hysteresis-banded AoI trigger for Witness
//
// Composes TWO underlying RangeTriggers centred on the same
// EntityRangeListNode:
//
//   inner — range = aoi_radius. Fires OnEnter → Witness::HandleAoIEnter
//           when a peer crosses inbound. Inner-band OnLeave is
//           SUPPRESSED — the peer stays in AoI until it also leaves the
//           outer band (that's the hysteresis window).
//
//   outer — range = aoi_radius + hysteresis. Fires OnLeave →
//           Witness::HandleAoILeave when a peer crosses outbound.
//           Outer-band OnEnter is SUPPRESSED — peers between outer and
//           inner are in the hysteresis window, not yet in AoI.
//
// This is strictly better than BigWorld's Witness::aoiHyst_, which is
// stored and serialized but never actually applied to the trigger
// boundary (see witness.cpp:2131 — setRange uses aoiRadius_ only, so a
// peer oscillating at the exact radius flaps enter/leave each tick).
//
// A peer oscillating at the exact aoi_radius boundary:
//   BigWorld     — enter/leave flap every tick the peer crosses.
//   Atlas (this) — ONE enter when the peer first crosses inner; stays
//                  in AoI as long as it stays within outer.
// ============================================================================

class AoITrigger {
 public:
  AoITrigger(Witness& witness, EntityRangeListNode& central, float inner_range, float outer_range);
  ~AoITrigger();

  AoITrigger(const AoITrigger&) = delete;
  auto operator=(const AoITrigger&) -> AoITrigger& = delete;

  // Insert both inner + outer bound pairs into the RangeList. Peers
  // already 2-D inside at insertion time fire inner's OnEnter (dispatched
  // to the witness as AoI enters) and outer's OnEnter (suppressed).
  void Insert(RangeList& list);

  // Remove both inner + outer bound pairs. Fires no OnLeave events —
  // mirror of RangeTrigger::Remove's contract.
  void Remove(RangeList& list);

  // Resize both bands in place. The caller computes the absolute
  // outer = inner + hysteresis and passes both explicitly.
  void SetBounds(float inner_range, float outer_range);

  [[nodiscard]] auto InnerRange() const -> float;
  [[nodiscard]] auto OuterRange() const -> float;

 private:
  class InnerTrigger;
  class OuterTrigger;

  std::unique_ptr<InnerTrigger> inner_;
  std::unique_ptr<OuterTrigger> outer_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_AOI_TRIGGER_H_
