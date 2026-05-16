#ifndef ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_MANAGER_H_
#define ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_MANAGER_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

#include "AtlasCore/client_entity.h"
#include "AtlasCore/entity_id.h"
#include "AtlasCore/entity_view.h"

namespace atlas {

class ClientEntityManager {
 public:
  using Factory = std::function<std::unique_ptr<ClientEntity>(EntityId, EntityTypeId)>;

  ClientEntityManager() = default;
  ~ClientEntityManager() = default;

  ClientEntityManager(const ClientEntityManager&) = delete;
  ClientEntityManager& operator=(const ClientEntityManager&) = delete;

  // Registered before any HandleEnter for that type — Subsystem hooks this up
  // before authentication completes (Task #9 in M0).
  void RegisterFactory(EntityTypeId type_id, Factory factory);

  // Takes ownership; rejects null, kInvalidEntityId, or duplicate id.
  bool Register(std::unique_ptr<ClientEntity> entity);

  bool Remove(EntityId id);

  [[nodiscard]] ClientEntity* Find(EntityId id) const;

  void Clear();

  [[nodiscard]] std::size_t Size() const { return entities_.size(); }

  // Spawns and registers a fresh entity via the type's factory, then seeds the
  // initial position into it. Returns false if id is invalid, already exists,
  // or no factory was registered for type_id.
  bool HandleEnter(EntityId id, EntityTypeId type_id, double server_time,
                   const Vec3& pos, const Vec3& dir, bool on_ground);

  // Equivalent to Remove(id) — present so envelope dispatch reads symmetrically.
  void HandleLeave(EntityId id) { Remove(id); }

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
  std::unordered_map<EntityTypeId, Factory> factories_;
};

}  // namespace atlas

#endif  // ATLAS_UE_CLIENT_CORE_CLIENT_ENTITY_MANAGER_H_
