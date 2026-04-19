#ifndef ATLAS_SERVER_CELLAPP_OFFLOAD_CHECKER_H_
#define ATLAS_SERVER_CELLAPP_OFFLOAD_CHECKER_H_

#include <vector>

#include "cellappmgr/bsp_tree.h"
#include "network/address.h"

namespace atlas {

class CellEntity;
class Space;

// ============================================================================
// OffloadChecker — per-tick border-crossing detection.
//
// Phase 11 §3.4. For each Real entity in a Space's local Cells, ask the
// authoritative BSP tree which Cell covers the entity's current
// position. If that Cell lives on a peer CellApp, emit an OffloadOp.
// CellApp consumes the list and drives the Offload handshake.
//
// Offload is suppressed when the Cell has `should_offload_ == false` —
// CellAppMgr uses that flag to freeze migrations during sensitive
// transitions (e.g. a BSP rebalance in flight).
// ============================================================================

class OffloadChecker {
 public:
  struct OffloadOp {
    CellEntity* entity;
    Address target_cellapp_addr;
    cellappmgr::CellID target_cell_id{0};
  };

  explicit OffloadChecker(Address self_addr);

  auto Compute(Space& space) const -> std::vector<OffloadOp>;

 private:
  Address self_addr_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_OFFLOAD_CHECKER_H_
