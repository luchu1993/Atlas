#include "movement_state_store.h"

namespace atlas {

auto MovementStateStore::Ensure(EntityID entity_id, const math::Vector3& position,
                                const math::Vector3& direction, bool on_ground)
    -> movement::MovementState& {
  auto [it, inserted] = states_.try_emplace(entity_id);
  if (inserted) {
    auto& state = it->second;
    state.position = position;
    state.direction = direction;
    state.flags = on_ground ? movement::kMovementFlagGrounded : 0u;
  }
  return it->second;
}

auto MovementStateStore::Find(EntityID entity_id) -> movement::MovementState* {
  auto it = states_.find(entity_id);
  return it == states_.end() ? nullptr : &it->second;
}

auto MovementStateStore::Find(EntityID entity_id) const -> const movement::MovementState* {
  auto it = states_.find(entity_id);
  return it == states_.end() ? nullptr : &it->second;
}

void MovementStateStore::Erase(EntityID entity_id) {
  states_.erase(entity_id);
}

void MovementStateStore::AppendEntityIds(std::vector<EntityID>& out) const {
  out.reserve(out.size() + states_.size());
  for (const auto& entry : states_) out.push_back(entry.first);
}

}  // namespace atlas
