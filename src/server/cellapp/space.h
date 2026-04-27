#ifndef ATLAS_SERVER_CELLAPP_SPACE_H_
#define ATLAS_SERVER_CELLAPP_SPACE_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "cellappmgr/bsp_tree.h"
#include "cellappmgr/cellappmgr_messages.h"  // CellID
#include "server/entity_types.h"
#include "space/range_list.h"

namespace atlas {

class Cell;
class CellEntity;

// Space — a self-contained spatial partition.
//
// Authority for:
//   - the collection of CellEntity instances currently living here
//   - the RangeList indexing those entities on (x, z)
//
// Does NOT know about:
//   - client sessions, RPC routing — those are BaseApp / Witness concerns
//   - terrain, physics — deferred

class Space {
 public:
  explicit Space(SpaceID id);
  ~Space();

  Space(const Space&) = delete;
  auto operator=(const Space&) -> Space& = delete;

  [[nodiscard]] auto Id() const -> SpaceID { return id_; }

  // Take ownership; returns the raw pointer for convenience. Entity is
  // inserted into the RangeList by its own constructor; AddEntity merely
  // parks it in the id→entity map.
  auto AddEntity(std::unique_ptr<CellEntity> entity) -> CellEntity*;

  // Tears the entity down (calls Destroy, which pulls range_node_ out
  // of the list) then erases the map entry. Safe to call multiple times
  // on the same id.
  void RemoveEntity(EntityID id);

  [[nodiscard]] auto FindEntity(EntityID id) -> CellEntity*;
  [[nodiscard]] auto FindEntity(EntityID id) const -> const CellEntity*;
  [[nodiscard]] auto EntityCount() const -> std::size_t { return entities_.size(); }

  [[nodiscard]] auto GetRangeList() -> RangeList& { return range_list_; }
  [[nodiscard]] auto GetRangeList() const -> const RangeList& { return range_list_; }

  // ---- Cells + BSP geometry ---------------------------------------
  //
  // `local_cells_` holds the subset of Cells authoritative on THIS
  // CellApp (indexed by CellID assigned by CellAppMgr). Ghost-only
  // Spaces have an empty local_cells_ — the Space still exists here
  // because its Real(s) are ghosted into it.
  //
  // `bsp_tree_` is the authoritative whole-Space partition. CellAppMgr
  // pushes it via UpdateGeometry and the Space replays it after each
  // update. Consumed read-only by GhostMaintainer and OffloadChecker.

  using LocalCellMap = std::unordered_map<cellappmgr::CellID, std::unique_ptr<Cell>>;

  auto AddLocalCell(std::unique_ptr<Cell> cell) -> Cell*;
  auto RemoveLocalCell(cellappmgr::CellID id) -> bool;
  [[nodiscard]] auto FindLocalCell(cellappmgr::CellID id) -> Cell*;
  [[nodiscard]] auto FindLocalCell(cellappmgr::CellID id) const -> const Cell*;
  [[nodiscard]] auto LocalCells() -> LocalCellMap& { return local_cells_; }
  [[nodiscard]] auto LocalCells() const -> const LocalCellMap& { return local_cells_; }

  void SetBspTree(BSPTree tree);
  [[nodiscard]] auto GetBspTree() -> BSPTree* {
    return bsp_tree_.has_value() ? &*bsp_tree_ : nullptr;
  }
  [[nodiscard]] auto GetBspTree() const -> const BSPTree* {
    return bsp_tree_.has_value() ? &*bsp_tree_ : nullptr;
  }

  // Drive every entity's per-tick work: controllers first (may alter
  // position), then a compaction pass on destroyed entities. Does NOT
  // drive Witness updates — that's a later stage of the CellApp tick.
  void Tick(float dt);

  // Run `fn(entity)` over every live entity. Safe to call during tick
  // outside of Witness::Update.
  template <typename Fn>
  void ForEachEntity(Fn&& fn) {
    for (auto& [_, entity] : entities_) {
      fn(*entity);
    }
  }

  // True from the moment ~Space starts unwinding — per-entity
  // destructors fire as the entities_ map is being torn down, and
  // iterating it during that window is UB.  Helpers that walk the
  // map from inside ~CellEntity should bail out when this is set.
  [[nodiscard]] auto IsTearingDown() const -> bool { return tearing_down_; }

 private:
  SpaceID id_;

  // Map owns the entities; RangeList holds non-owning pointers to the
  // embedded range_node_. Destruction order matters — entities must be
  // destroyed BEFORE the RangeList so range_node_ unlinks cleanly. Field
  // order below achieves that (RangeList destructs last).
  RangeList range_list_;
  std::unordered_map<EntityID, std::unique_ptr<CellEntity>> entities_;

  // Cells are non-owning wrt entities (the map above owns); they just
  // track membership. Declared AFTER entities_ so their destructors
  // run first — Cell does not touch entity state at teardown, but
  // this ordering keeps subsequent additions safe by default.
  LocalCellMap local_cells_;
  std::optional<BSPTree> bsp_tree_;

  bool tearing_down_{false};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_SPACE_H_
