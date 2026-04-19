#include "cell.h"

#include <algorithm>

#include "cell_entity.h"

namespace atlas {

Cell::Cell(Space& space, cellappmgr::CellID cell_id, const CellBounds& bounds)
    : space_(space), cell_id_(cell_id), bounds_(bounds) {}

void Cell::AddRealEntity(CellEntity* entity) {
  if (entity == nullptr) return;
  // Duplicate-add is silently dropped. Callers that truly mean to re-link
  // an entity must RemoveRealEntity first — that makes the membership
  // transition explicit in logs.
  if (HasRealEntity(entity)) return;
  real_entities_.push_back(entity);
}

auto Cell::RemoveRealEntity(CellEntity* entity) -> bool {
  auto it = std::find(real_entities_.begin(), real_entities_.end(), entity);
  if (it == real_entities_.end()) return false;
  *it = real_entities_.back();  // swap-back keeps remove O(1).
  real_entities_.pop_back();
  return true;
}

auto Cell::HasRealEntity(const CellEntity* entity) const -> bool {
  return std::find(real_entities_.begin(), real_entities_.end(), entity) != real_entities_.end();
}

}  // namespace atlas
