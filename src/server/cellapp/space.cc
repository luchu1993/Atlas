#include "space.h"

#include <utility>

#include "cell.h"
#include "cell_entity.h"
#include "foundation/profiler.h"

namespace atlas {

Space::Space(SpaceID id) : id_(id) {}

// Out-of-line destructor: CellEntity (and Cell) are incomplete in
// space.h (forward declared to break the include cycle), so the
// unique_ptr deleter can't be instantiated inline there.  We also
// flip tearing_down_ so per-entity destructors that introspect
// space state (e.g. ~CellEntity's leave-fanout audit) can short-
// circuit before the map iteration races our teardown.
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
  // Controllers first so motion changes propagate into the RangeList
  // before any post-tick Witness pass reads it.
  //
  // Intentionally NO compaction pass here. The only path that sets
  // IsDestroyed is RemoveEntity, which erases the entity from
  // `entities_` synchronously. A dedicated compaction sweep would be a
  // second destruction path that bypasses CellApp's base/cell entity
  // indexes (`entity_population_` / `base_entity_population_` in
  // cellapp.h), turning them into stale pointers. If deferred
  // destruction is ever needed (e.g. destroy-during-controller-update),
  // add it with a CellApp-visible notification hook — not a silent
  // Space-local compaction.
  for (auto& [_, entity] : entities_) {
    if (!entity->IsDestroyed()) entity->GetControllers().Update(dt);
  }
}

}  // namespace atlas
