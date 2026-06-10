#include "space.h"

#include <utility>

#include "cell.h"
#include "cell_entity.h"
#include "foundation/log.h"
#include "foundation/profiler.h"
#include "navigation/nav_backend.h"
#include "navigation/nav_input.h"
#include "navigation/nav_params.h"
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
  collision_asset_object_count_ = asset.boxes.size() + asset.planes.size() +
                                  asset.spheres.size() + asset.capsules.size() +
                                  asset.convexes.size();
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
    collision_asset_object_count_ = cache->asset.boxes.size() + cache->asset.planes.size() +
                                    cache->asset.spheres.size() + cache->asset.capsules.size() +
                                    cache->asset.meshes.size() + cache->asset.convexes.size() +
                                    cache->asset.heightfields.size();
    return {};
  }

  // No backend injected: the Static fallback only builds box / plane, so any
  // sphere / capsule / mesh / convex / heightfield has no representable backend
  // — reject rather than silently drop it.
  if (!cache->asset.spheres.empty() || !cache->asset.capsules.empty() ||
      !cache->asset.meshes.empty() || !cache->asset.convexes.empty() ||
      !cache->asset.heightfields.empty()) {
    return Error{ErrorCode::kNotSupported,
                 "collision cache has non-box/plane shapes but no physics backend is "
                 "configured (Static builds box/plane only)"};
  }
  SetCollisionAsset(cache->asset);
  return {};
}

void Space::SetNavQuery(std::unique_ptr<nav::NavQuery> query) {
  if (!query) return;
  nav_query_ = std::move(query);
  nav_source_hash_.clear();
}

void Space::SetNavBackendFactory(std::shared_ptr<const nav::NavBackendFactory> factory) {
  nav_backend_factory_ = std::move(factory);
}

auto Space::LoadNavMeshFromFiles(const std::filesystem::path& collision_path,
                                 const std::filesystem::path& params_path) -> Result<void> {
  if (nav_backend_factory_ == nullptr) {
    return Error{ErrorCode::kNotSupported,
                 "no nav backend is configured (build with ATLAS_ENABLE_RECAST)"};
  }
  auto asset = physics::LoadCollisionAssetFromFile(collision_path);
  if (!asset) return asset.Error();
  auto params = nav::LoadNavParamsFromFile(params_path);
  if (!params) return params.Error();
  auto derived = nav::DeriveNavInput(*asset, *params);
  for (const auto& warning : derived.stats.warnings) {
    ATLAS_LOG_WARNING("Space {}: nav derive: {}", id_, warning);
  }
  auto query = nav_backend_factory_->Bake(derived.geometry, params->bake);
  if (!query) return query.Error();
  nav_query_ = std::move(*query);
  nav_source_hash_ = params->source_hash;
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
