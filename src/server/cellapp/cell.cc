#include "cell.h"

#include <algorithm>

#include "cell_entity.h"

namespace atlas {

Cell::Cell(Space& space, cellappmgr::CellID cell_id, const CellBounds& bounds)
    : space_(space), cell_id_(cell_id), bounds_(bounds) {}

void Cell::AddRealEntity(CellEntity* entity) {
  if (entity == nullptr) return;
  // Duplicate-add is a no-op; relink callers must RemoveRealEntity first.
  if (HasRealEntity(entity)) return;
  real_entities_.push_back(entity);
}

auto Cell::RemoveRealEntity(CellEntity* entity) -> bool {
  auto it = std::find(real_entities_.begin(), real_entities_.end(), entity);
  if (it == real_entities_.end()) return false;
  *it = real_entities_.back();  // swap-back: O(1) remove
  real_entities_.pop_back();
  return true;
}

auto Cell::HasRealEntity(const CellEntity* entity) const -> bool {
  return std::find(real_entities_.begin(), real_entities_.end(), entity) != real_entities_.end();
}

}  // namespace atlas
