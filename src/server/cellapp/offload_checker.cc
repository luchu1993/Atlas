#include "offload_checker.h"

#include "cell.h"
#include "cell_entity.h"
#include "space.h"

namespace atlas {

OffloadChecker::OffloadChecker(Address self_addr) : self_addr_(self_addr) {}

auto OffloadChecker::Compute(Space& space) const -> std::vector<OffloadOp> {
  std::vector<OffloadOp> ops;
  const auto* bsp = space.GetBspTree();
  if (bsp == nullptr) return ops;  // No geometry yet → nothing to migrate.

  // Walk local Cells' Real entities. A Cell with should_offload_ == false
  // is opted out of migration this tick (CellAppMgr toggle during
  // rebalance).
  for (auto& [_, cell] : space.LocalCells()) {
    if (!cell->ShouldOffload()) continue;
    for (auto* entity : cell->RealEntities()) {
      if (!entity->IsReal()) continue;  // Defensive; should always hold here.
      const auto& pos = entity->Position();
      const auto* info = bsp->FindCell(pos.x, pos.z);
      if (info == nullptr) continue;                   // Outside any cell — odd, skip.
      if (info->cellapp_addr == self_addr_) continue;  // Still ours.
      ops.push_back(OffloadOp{entity, info->cellapp_addr, info->cell_id});
    }
  }
  return ops;
}

}  // namespace atlas
