#ifndef ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_MANAGER_H_
#define ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_MANAGER_H_

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>

#include "AtlasCore/client_entity.h"
#include "AtlasCore/entity_id.h"
#include "AtlasCore/entity_view.h"

namespace atlas {

class ClientEntityManager {
 public:
  ClientEntityManager() = default;
  ~ClientEntityManager() = default;

  ClientEntityManager(const ClientEntityManager&) = delete;
  ClientEntityManager& operator=(const ClientEntityManager&) = delete;

  // Takes ownership; rejects null, kInvalidEntityId, or duplicate id.
  bool Register(std::unique_ptr<ClientEntity> entity);

  bool Remove(EntityId id);

  [[nodiscard]] ClientEntity* Find(EntityId id) const;

  void Clear();

  [[nodiscard]] std::size_t Size() const { return entities_.size(); }

  // Routes a position envelope to the addressed entity; unknown id is silently
  // dropped (server may target an entity that just left the client's AoI).
  void HandlePositionUpdate(EntityId id, double server_time, const Vec3& pos,
                            const Vec3& dir, bool on_ground);

  void TickAll(double dt);

  template <typename F>
  void ForEach(F&& fn) const {
    for (const auto& [id, entity] : entities_) {
      fn(*entity);
    }
  }

 private:
  std::unordered_map<EntityId, std::unique_ptr<ClientEntity>> entities_;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_MANAGER_H_
