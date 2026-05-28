#ifndef ATLAS_SERVER_CELLAPP_MOVEMENT_STATE_STORE_H_
#define ATLAS_SERVER_CELLAPP_MOVEMENT_STATE_STORE_H_

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "math/vector3.h"
#include "movement_sim/movement_sim.h"
#include "server/entity_types.h"

namespace atlas {

class MovementStateStore {
 public:
  [[nodiscard]] auto Ensure(EntityID entity_id, const math::Vector3& position,
                            const math::Vector3& direction, bool on_ground)
      -> movement::MovementState&;
  [[nodiscard]] auto Find(EntityID entity_id) -> movement::MovementState*;
  [[nodiscard]] auto Find(EntityID entity_id) const -> const movement::MovementState*;
  void Erase(EntityID entity_id);
  void AppendEntityIds(std::vector<EntityID>& out) const;

  [[nodiscard]] auto Size() const -> std::size_t { return states_.size(); }

 private:
  std::unordered_map<EntityID, movement::MovementState> states_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_MOVEMENT_STATE_STORE_H_
