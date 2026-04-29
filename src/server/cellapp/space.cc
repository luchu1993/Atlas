#include "space.h"

#include <utility>

#include "cell.h"
#include "cell_entity.h"
#include "foundation/profiler.h"

namespace atlas {

Space::Space(SpaceID id) : id_(id) {}

// Out-of-line because CellEntity/Cell are forward-declared in space.h.
// Sets tearing_down_ so ~CellEntity helpers can short-circuit map walks.
Space::~Space() {
  tearing_down_ = true;
}

auto Space::AddLocalCell(std::unique_ptr<Cell> cell) -> Cell* {
  auto* raw = cell.get();
  const auto id = cell->Id();
  local_cells_[id] = std::move(cell);
  return raw;
}

auto Space::RemoveLocalCell(cellappmgr::CellID id) -> bool {
  return local_cells_.erase(id) > 0;
}

auto Space::FindLocalCell(cellappmgr::CellID id) -> Cell* {
  auto it = local_cells_.find(id);
  return it == local_cells_.end() ? nullptr : it->second.get();
}

auto Space::FindLocalCell(cellappmgr::CellID id) const -> const Cell* {
  auto it = local_cells_.find(id);
  return it == local_cells_.end() ? nullptr : it->second.get();
}

void Space::SetBspTree(BSPTree tree) {
  bsp_tree_ = std::move(tree);
}

auto Space::AddEntity(std::unique_ptr<CellEntity> entity) -> CellEntity* {
  auto* raw = entity.get();
  const EntityID id = entity->Id();
  entities_.emplace(id, std::move(entity));
  return raw;
}

void Space::RemoveEntity(EntityID id) {
  auto it = entities_.find(id);
  if (it == entities_.end()) return;
  it->second->Destroy();
  entities_.erase(it);
}

auto Space::FindEntity(EntityID id) -> CellEntity* {
  auto it = entities_.find(id);
  return it == entities_.end() ? nullptr : it->second.get();
}

auto Space::FindEntity(EntityID id) const -> const CellEntity* {
  auto it = entities_.find(id);
  return it == entities_.end() ? nullptr : it->second.get();
}

void Space::Tick(float dt) {
  ATLAS_PROFILE_ZONE_N("Space::Tick");
  // Controllers run before any Witness pass reads RangeList.
  // No compaction here: RemoveEntity is the sole destruction path and
  // erases synchronously. A second sweep would bypass CellApp's
  // entity_population_ / base_entity_population_ indexes.
  for (auto& [_, entity] : entities_) {
    if (!entity->IsDestroyed()) entity->GetControllers().Update(dt);
  }
}

}  // namespace atlas
