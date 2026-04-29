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

// Self-contained spatial partition; owns the CellEntity collection and the
// (x, z) RangeList. No client/RPC/physics knowledge.
class Space {
 public:
  explicit Space(SpaceID id);
  ~Space();

  Space(const Space&) = delete;
  auto operator=(const Space&) -> Space& = delete;

  [[nodiscard]] auto Id() const -> SpaceID { return id_; }

  // Entity inserts itself into RangeList in its ctor; AddEntity just
  // parks it in the id→entity map.
  auto AddEntity(std::unique_ptr<CellEntity> entity) -> CellEntity*;

  // Idempotent on missing id.
  void RemoveEntity(EntityID id);

  [[nodiscard]] auto FindEntity(EntityID id) -> CellEntity*;
  [[nodiscard]] auto FindEntity(EntityID id) const -> const CellEntity*;
  [[nodiscard]] auto EntityCount() const -> std::size_t { return entities_.size(); }

  [[nodiscard]] auto GetRangeList() -> RangeList& { return range_list_; }
  [[nodiscard]] auto GetRangeList() const -> const RangeList& { return range_list_; }

  // local_cells_: Cells authoritative here (empty for ghost-only Spaces).
  // bsp_tree_: whole-Space partition pushed by CellAppMgr; read-only for
  // GhostMaintainer / OffloadChecker.
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

  // Controllers (may alter position) then dead-entity compaction.
  // Witness updates run later in the CellApp tick.
  void Tick(float dt);

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

  // Non-owning; declared after entities_ so cells destruct first (safe
  // default even though Cell doesn't touch entity state at teardown).
  LocalCellMap local_cells_;
  std::optional<BSPTree> bsp_tree_;

  bool tearing_down_{false};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_SPACE_H_
