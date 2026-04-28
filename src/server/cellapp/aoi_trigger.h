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
// A peer oscillating at the exact aoi_radius boundary gets ONE enter
// when it first crosses inner and stays in AoI as long as it stays
// within outer — no per-tick enter/leave flap.
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

  // Resync both bound pairs against the central's NEW position.  Must be
  // called by the witness owner after the central's range_node has
  // shuffled — bound nodes are tied to central via X()/Z() but their
  // list-position reflects the OLD central, so their inside_peers_ stay
  // stale until each bound shuffles to its new sorted slot.  Without
  // this hook a moving observer's outer.OnLeave never fires for peers
  // that drift out of range, leaking dangling pointers in aoi_map_.
  void OnCentralMoved(float old_central_x, float old_central_z);

  // Force-insert a peer into outer band's inside_peers_.  Called by
  // InnerTrigger.OnEnter to keep the inner ⊂ outer invariant intact
  // when cross-event ordering would otherwise leave outer stale.
  void ForceOuterInsidePeer(class RangeListNode& peer);

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
