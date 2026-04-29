#ifndef ATLAS_SERVER_CELLAPP_OFFLOAD_CHECKER_H_
#define ATLAS_SERVER_CELLAPP_OFFLOAD_CHECKER_H_

#include <vector>

#include "cellappmgr/bsp_tree.h"
#include "network/address.h"

namespace atlas {

class CellEntity;
class Space;

// Per-tick border-crossing detection: emits an OffloadOp when the
// authoritative BSP tree assigns an entity to a peer CellApp's Cell.
// Skipped when the source Cell has should_offload_ == false (CellAppMgr
// freezes migrations during BSP rebalance).
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
