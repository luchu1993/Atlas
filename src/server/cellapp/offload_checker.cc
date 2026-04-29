#include "offload_checker.h"

#include "cell.h"
#include "cell_entity.h"
#include "space.h"

namespace atlas {

OffloadChecker::OffloadChecker(Address self_addr) : self_addr_(self_addr) {}

auto OffloadChecker::Compute(Space& space) const -> std::vector<OffloadOp> {
  std::vector<OffloadOp> ops;
  const auto* bsp = space.GetBspTree();
  if (bsp == nullptr) return ops;

  for (auto& [_, cell] : space.LocalCells()) {
    if (!cell->ShouldOffload()) continue;
    for (auto* entity : cell->RealEntities()) {
      if (!entity->IsReal()) continue;
      const auto& pos = entity->Position();
      const auto* info = bsp->FindCell(pos.x, pos.z);
      if (info == nullptr) continue;
      if (info->cellapp_addr == self_addr_) continue;
      ops.push_back(OffloadOp{entity, info->cellapp_addr, info->cell_id});
    }
  }
  return ops;
}

}  // namespace atlas
