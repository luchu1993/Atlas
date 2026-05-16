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

  // Must be registered before any HandleEnter for that type.
  void RegisterFactory(EntityTypeId type_id, Factory factory);

  // Takes ownership; rejects null, kInvalidEntityId, or duplicate id.
  bool Register(std::unique_ptr<ClientEntity> entity);

  bool Remove(EntityId id);

  [[nodiscard]] ClientEntity* Find(EntityId id) const;

  void Clear();

  [[nodiscard]] std::size_t Size() const { return entities_.size(); }

  // Like HandleEnter but skips position seeding — for entities (e.g. Account)
  // that arrive via owner handoff without spatial state.
  bool HandleCreate(EntityId id, EntityTypeId type_id);

  // Returns false on invalid id, duplicate id, or no registered factory.
  bool HandleEnter(EntityId id, EntityTypeId type_id, double server_time,
                   const Vec3& pos, const Vec3& dir, bool on_ground);

  void HandleLeave(EntityId id) { Remove(id); }

  // Unknown id is silently dropped — server may target an entity that just
  // left the client's AoI.
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
