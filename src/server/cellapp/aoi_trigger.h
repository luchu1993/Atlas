#ifndef ATLAS_SERVER_CELLAPP_AOI_TRIGGER_H_
#define ATLAS_SERVER_CELLAPP_AOI_TRIGGER_H_

#include <memory>

#include "space/range_trigger.h"

namespace atlas {

class CellEntity;
class EntityRangeListNode;
class RangeList;
class Witness;

// Hysteresis-banded AoI trigger for Witness. Inner band (aoi_radius) fires
// OnEnter; outer band (aoi_radius + hysteresis) fires OnLeave. Peers in
// the gap stay in AoI but don't toggle. Eliminates per-tick boundary flap.
class AoITrigger {
 public:
  AoITrigger(Witness& witness, EntityRangeListNode& central, float inner_range, float outer_range);
  ~AoITrigger();

  AoITrigger(const AoITrigger&) = delete;
  auto operator=(const AoITrigger&) -> AoITrigger& = delete;

  // Inserts both bands; existing-peers fire inner.OnEnter (delivered as
  // AoI enter); outer.OnEnter is suppressed.
  void Insert(RangeList& list);

  // Remove both bands; no OnLeave fires (mirrors RangeTrigger::Remove).
  void Remove(RangeList& list);

  // Caller computes outer = inner + hysteresis.
  void SetBounds(float inner_range, float outer_range);

  // Required after central's range_node shuffles: bound nodes still
  // reflect the OLD central position, so inside_peers_ stay stale and
  // outer.OnLeave fails to fire — leaks dangling aoi_map_ pointers.
  void OnCentralMoved(float old_central_x, float old_central_z);

  // Maintains inner ⊂ outer invariant when cross-event ordering would
  // otherwise leave outer's inside_peers_ stale.
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
