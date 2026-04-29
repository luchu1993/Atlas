#ifndef ATLAS_SERVER_CELLAPP_CELL_H_
#define ATLAS_SERVER_CELLAPP_CELL_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cell_bounds.h"
#include "cellappmgr/cellappmgr_messages.h"  // CellID

namespace atlas {

class Space;
class CellEntity;

// Sub-region of a Space hosted on this CellApp; one Space can span many
// Cells across CellApps. Holds non-owning pointers (Space owns entities)
// with O(1) swap-back removal.
class Cell {
 public:
  Cell(Space& space, cellappmgr::CellID cell_id, const CellBounds& bounds);

  [[nodiscard]] auto GetSpace() -> Space& { return space_; }
  [[nodiscard]] auto GetSpace() const -> const Space& { return space_; }
  [[nodiscard]] auto Id() const -> cellappmgr::CellID { return cell_id_; }

  // SetBounds rewrites in place; OffloadChecker re-partitions next tick.
  [[nodiscard]] auto Bounds() const -> const CellBounds& { return bounds_; }
  void SetBounds(const CellBounds& b) { bounds_ = b; }

  // Membership is explicit — never inferred from bounds, so a tick-mid
  // excursion does not drop the entity. Callers wire these on every
  // arrival (Create, Offload-in, Ghost→Real) and departure.
  void AddRealEntity(CellEntity* entity);
  auto RemoveRealEntity(CellEntity* entity) -> bool;
  [[nodiscard]] auto HasRealEntity(const CellEntity* entity) const -> bool;
  [[nodiscard]] auto RealEntityCount() const -> std::size_t { return real_entities_.size(); }
  [[nodiscard]] auto RealEntities() const -> const std::vector<CellEntity*>& {
    return real_entities_;
  }

  template <typename Fn>
  void ForEachRealEntity(Fn&& fn) {
    for (auto* e : real_entities_) fn(*e);
  }

  // Toggled by CellAppMgr; fresh cells default to true.
  [[nodiscard]] auto ShouldOffload() const -> bool { return should_offload_; }
  void SetShouldOffload(bool v) { should_offload_ = v; }

 private:
  Space& space_;
  cellappmgr::CellID cell_id_;
  CellBounds bounds_;
  std::vector<CellEntity*> real_entities_;
  bool should_offload_{true};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELL_H_
