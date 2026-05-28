#ifndef ATLAS_SERVER_CELLAPP_MOVEMENT_COMMAND_STORE_H_
#define ATLAS_SERVER_CELLAPP_MOVEMENT_COMMAND_STORE_H_

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "movement_sim/movement_sim.h"
#include "server/entity_types.h"

namespace atlas {

class MovementCommandStore {
 public:
  [[nodiscard]] auto Set(EntityID entity_id, const movement::MovementCommand& command)
      -> bool;
  [[nodiscard]] auto Find(EntityID entity_id) -> movement::MovementCommand*;
  [[nodiscard]] auto Find(EntityID entity_id) const -> const movement::MovementCommand*;
  void Erase(EntityID entity_id);
  void AppendEntityIds(std::vector<EntityID>& out) const;

  [[nodiscard]] auto Size() const -> std::size_t { return commands_.size(); }

 private:
  std::unordered_map<EntityID, movement::MovementCommand> commands_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_MOVEMENT_COMMAND_STORE_H_
