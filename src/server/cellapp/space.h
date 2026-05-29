#ifndef ATLAS_SERVER_CELLAPP_SPACE_H_
#define ATLAS_SERVER_CELLAPP_SPACE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "cellappmgr/bsp_tree.h"
#include "cellappmgr/cellappmgr_messages.h"  // CellID
#include "foundation/clock.h"
#include "foundation/error.h"
#include "physics/physics_query.h"
#include "server/entity_types.h"
#include "space/range_list.h"
#include "space_data.h"

namespace atlas {

class Cell;
class CellEntity;
namespace physics {
struct CollisionAsset;
class CollisionBackendFactory;
}

// Self-contained spatial partition; owns CellEntities, RangeList, and physics.
class Space {
 public:
  explicit Space(SpaceID id);
  ~Space();

  Space(const Space&) = delete;
  auto operator=(const Space&) -> Space& = delete;

  [[nodiscard]] auto Id() const -> SpaceID { return id_; }

  // Entity inserts itself into RangeList in its ctor; AddEntity just
  // parks it in the id->entity map.
  auto AddEntity(std::unique_ptr<CellEntity> entity) -> CellEntity*;

  // Idempotent on missing id.
  void RemoveEntity(EntityID id);

  [[nodiscard]] auto FindEntity(EntityID id) -> CellEntity*;
  [[nodiscard]] auto FindEntity(EntityID id) const -> const CellEntity*;
  [[nodiscard]] auto EntityCount() const -> std::size_t { return entities_.size(); }

  [[nodiscard]] auto GetRangeList() -> RangeList& { return range_list_; }
  [[nodiscard]] auto GetRangeList() const -> const RangeList& { return range_list_; }

  // local_cells_: Cells authoritative here, empty for ghost-only Spaces.
  // bsp_tree_: whole-Space partition read by ghost and offload systems.
  using LocalCellMap = std::unordered_map<cellappmgr::CellID, std::unique_ptr<Cell>>;

  auto AddLocalCell(std::unique_ptr<Cell> cell) -> Cell*;
  auto RemoveLocalCell(cellappmgr::CellID id) -> bool;
  [[nodiscard]] auto FindLocalCell(cellappmgr::CellID id) -> Cell*;
  [[nodiscard]] auto FindLocalCell(cellappmgr::CellID id) const -> const Cell*;
  [[nodiscard]] auto LocalCells() -> LocalCellMap& { return local_cells_; }
  [[nodiscard]] auto LocalCells() const -> const LocalCellMap& { return local_cells_; }

  void SetBspTree(BSPTree tree, uint64_t geometry_version = 0);
  [[nodiscard]] auto GetBspTree() -> BSPTree* {
    return bsp_tree_.has_value() ? &*bsp_tree_ : nullptr;
  }
  [[nodiscard]] auto GetBspTree() const -> const BSPTree* {
    return bsp_tree_.has_value() ? &*bsp_tree_ : nullptr;
  }

  // True iff this cellapp holds the BSP primary (left-most) cell - the
  // SpaceData authority. False before SetBspTree has been called.
  [[nodiscard]] auto IsOwner() const -> bool;
  [[nodiscard]] auto GeometryVersion() const -> uint64_t { return geometry_version_; }

  [[nodiscard]] auto Data() -> SpaceData& { return data_; }
  [[nodiscard]] auto Data() const -> const SpaceData& { return data_; }
  [[nodiscard]] auto PhysicsQuery() -> physics::PhysicsQuery& { return *physics_query_; }
  [[nodiscard]] auto PhysicsQuery() const -> const physics::PhysicsQuery& {
    return *physics_query_;
  }
  void SetPhysicsQuery(std::unique_ptr<physics::PhysicsQuery> query);
  void SetCollisionAsset(const physics::CollisionAsset& asset);
  // Optional backend factory (Jolt). When set, cooked caches build through it;
  // when null, mesh-bearing caches are rejected rather than silently degraded.
  void SetCollisionBackendFactory(
      std::shared_ptr<const physics::CollisionBackendFactory> factory);
  [[nodiscard]] auto LoadCollisionAssetFromFile(const std::filesystem::path& path)
      -> Result<void>;
  [[nodiscard]] auto LoadCollisionCacheFromFile(const std::filesystem::path& path)
      -> Result<void>;
  [[nodiscard]] auto CollisionAssetSourceHash() const -> std::string_view {
    return collision_asset_source_hash_;
  }
  [[nodiscard]] auto CollisionAssetObjectCount() const -> std::size_t {
    return collision_asset_object_count_;
  }

  // True once SpaceData has been seeded - by becoming owner on SetBspTree,
  // or by receiving a SpaceDataSnapshot from the owner.
  [[nodiscard]] auto IsDataInitialized() const -> bool { return data_initialized_; }
  void MarkDataInitialized();
  void BeginPrimaryHandoffSnapshot(Address source_addr);
  [[nodiscard]] auto PendingSpaceDataSourceAddr() const -> Address {
    return pending_space_data_source_addr_;
  }

  // Controllers may alter position; Witness updates run later in the CellApp tick.
  void Tick(float dt);
  using ControllerTickObserver = std::function<void(CellEntity&, Duration)>;
  void Tick(float dt, const ControllerTickObserver& observer);

  template <typename Fn>
  void ForEachEntity(Fn&& fn) {
    for (auto& [_, entity] : entities_) {
      fn(*entity);
    }
  }

  // Set during ~Space; iterating entities_ then is UB. Helpers walking
  // the map from inside ~CellEntity must check this.
  [[nodiscard]] auto IsTearingDown() const -> bool { return tearing_down_; }

 private:
  SpaceID id_;

  // RangeList holds non-owning ptrs into entities_; declared FIRST so it
  // destructs LAST and range_node_ unlinks cleanly.
  RangeList range_list_;
  std::unordered_map<EntityID, std::unique_ptr<CellEntity>> entities_;

  // Declared after entities_ so cells destruct first by default.
  LocalCellMap local_cells_;
  std::optional<BSPTree> bsp_tree_;
  uint64_t geometry_version_{0};
  SpaceData data_;
  std::unique_ptr<physics::PhysicsQuery> physics_query_{
      std::make_unique<physics::StaticPhysicsQuery>()};
  std::shared_ptr<const physics::CollisionBackendFactory> collision_backend_factory_;
  std::string collision_asset_source_hash_;
  std::size_t collision_asset_object_count_{0};
  bool data_initialized_{false};
  Address pending_space_data_source_addr_;

  bool tearing_down_{false};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_SPACE_H_
