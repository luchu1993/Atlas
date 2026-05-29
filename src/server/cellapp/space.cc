#include "space.h"

#include <utility>

#include "cell.h"
#include "cell_entity.h"
#include "foundation/profiler.h"
#include "physics/collision_asset.h"
#include "physics/collision_backend.h"

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

void Space::SetBspTree(BSPTree tree, uint64_t geometry_version) {
  bsp_tree_ = std::move(tree);
  geometry_version_ = geometry_version;
  if (IsOwner() && pending_space_data_source_addr_.Port() == 0) data_initialized_ = true;
}

auto Space::IsOwner() const -> bool {
  if (!bsp_tree_.has_value()) return false;
  const auto primary = bsp_tree_->PrimaryCellId();
  if (primary == 0) return false;
  return local_cells_.count(primary) > 0;
}

void Space::MarkDataInitialized() {
  data_initialized_ = true;
  pending_space_data_source_addr_ = {};
}

void Space::BeginPrimaryHandoffSnapshot(Address source_addr) {
  pending_space_data_source_addr_ = source_addr;
  data_initialized_ = false;
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

void Space::SetPhysicsQuery(std::unique_ptr<physics::PhysicsQuery> query) {
  if (!query) return;
  physics_query_ = std::move(query);
  collision_asset_source_hash_.clear();
  collision_asset_object_count_ = 0;
}

void Space::SetCollisionAsset(const physics::CollisionAsset& asset) {
  auto query = physics::BuildStaticPhysicsQueryFromAsset(asset);
  physics_query_ = std::move(query);
  collision_asset_source_hash_ = asset.source_hash;
  collision_asset_object_count_ = asset.boxes.size() + asset.planes.size();
}

void Space::SetCollisionBackendFactory(
    std::shared_ptr<const physics::CollisionBackendFactory> factory) {
  collision_backend_factory_ = std::move(factory);
}

auto Space::LoadCollisionAssetFromFile(const std::filesystem::path& path) -> Result<void> {
  auto asset = physics::LoadCollisionAssetFromFile(path);
  if (!asset) return asset.Error();
  SetCollisionAsset(*asset);
  return {};
}

auto Space::LoadCollisionCacheFromFile(const std::filesystem::path& path) -> Result<void> {
  auto cache = physics::LoadCollisionCacheFromFile(path);
  if (!cache) return cache.Error();

  if (collision_backend_factory_ != nullptr) {
    auto query = collision_backend_factory_->BuildFromCache(*cache);
    if (!query) return query.Error();
    physics_query_ = std::move(*query);
    collision_asset_source_hash_ = cache->asset.source_hash;
    collision_asset_object_count_ =
        cache->asset.boxes.size() + cache->asset.planes.size() + cache->asset.meshes.size();
    return {};
  }

  // No backend injected: box/plane caches still build a Static query, but a
  // mesh-bearing cache has no representable backend — reject, don't degrade.
  if (!cache->asset.meshes.empty()) {
    return Error{ErrorCode::kNotSupported,
                 "collision cache contains meshes but no physics backend is configured"};
  }
  SetCollisionAsset(cache->asset);
  return {};
}

void Space::Tick(float dt) {
  Tick(dt, ControllerTickObserver{});
}

void Space::Tick(float dt, const ControllerTickObserver& observer) {
  ATLAS_PROFILE_ZONE_N("Space::Tick");
  // Controllers run before Witness reads RangeList; RemoveEntity owns erasure.
  // A tick-time compaction pass would bypass CellApp's population index.
  for (auto& [_, entity] : entities_) {
    if (entity->IsDestroyed()) continue;
    const auto started_at = observer ? Clock::now() : TimePoint{};
    entity->GetControllers().Update(dt);
    if (observer) observer(*entity, Clock::now() - started_at);
  }
}

}  // namespace atlas
