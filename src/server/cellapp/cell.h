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

// ============================================================================
// Cell — a Space's sub-region hosted by this CellApp.
//
// Phase 11 §3.6. A Phase 10 Space maps 1:1 onto a Cell; Phase 11 lets one
// Space span multiple Cells across several CellApps. A Cell tracks its
// slice of the BSP partition (`bounds_`), the live Real entities whose
// authoritative home is here, and the CellAppMgr-controlled offload
// enable flag (ShouldOffload, Phase 11 §2.3).
//
// This class intentionally does NOT own the entities — the Space's
// `entities_` map owns the unique_ptrs. Cell holds non-owning pointers
// keyed by insertion order, removed via swap-back for O(1) membership
// updates.
// ============================================================================

class Cell {
 public:
  Cell(Space& space, cellappmgr::CellID cell_id, const CellBounds& bounds);

  [[nodiscard]] auto GetSpace() -> Space& { return space_; }
  [[nodiscard]] auto GetSpace() const -> const Space& { return space_; }
  [[nodiscard]] auto Id() const -> cellappmgr::CellID { return cell_id_; }

  // UpdateGeometry (Phase 11 §2.3) rewrites bounds in-place. Does NOT
  // re-partition the entity list — OffloadChecker reacts on the next
  // tick by detecting entities that now fall outside `bounds_`.
  [[nodiscard]] auto Bounds() const -> const CellBounds& { return bounds_; }
  void SetBounds(const CellBounds& b) { bounds_ = b; }

  // Real entities hosted by this Cell. Callers must call these whenever a
  // Real arrives (CreateCellEntity, Offload arrival, Ghost→Real
  // conversion) or leaves (Destroy, Offload departure, Real→Ghost
  // conversion). Cell never infers entity ownership from bounds alone;
  // explicit book-keeping is required so that a brief excursion outside
  // bounds (during tick work, before OffloadChecker runs) doesn't drop
  // the entity from the membership list.
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

  // Offload enable flag, toggled by CellAppMgr via ShouldOffload msg.
  // Default true — fresh cells accept offload until told otherwise.
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
