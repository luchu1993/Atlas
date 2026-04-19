#include "space.h"

#include "cell_entity.h"

namespace atlas {

Space::Space(SpaceID id) : id_(id) {}

// Out-of-line destructor: CellEntity is incomplete in space.h (forward
// declared to break the include cycle), so the unique_ptr deleter can't
// be instantiated inline there.
Space::~Space() = default;

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
  // Controllers first so motion changes propagate into the RangeList
  // before any post-tick Witness pass reads it.
  //
  // Intentionally NO compaction pass here. The only path that sets
  // IsDestroyed is RemoveEntity, which erases the entity from
  // `entities_` synchronously. A dedicated compaction sweep would be a
  // second destruction path that bypasses CellApp's base/cell entity
  // indexes (`entity_population_` / `base_entity_population_` in
  // cellapp.h), turning them into stale pointers. If Phase 11 ever
  // needs deferred destruction (e.g. destroy-during-controller-update),
  // add it with a CellApp-visible notification hook — not a silent
  // Space-local compaction.
  for (auto& [_, entity] : entities_) {
    if (!entity->IsDestroyed()) entity->GetControllers().Update(dt);
  }
}

}  // namespace atlas
