#include "AtlasCore/client_entity_manager.h"

#include <utility>

namespace atlas {

void ClientEntityManager::RegisterFactory(EntityTypeId type_id, Factory factory) {
  factories_[type_id] = std::move(factory);
}

bool ClientEntityManager::Register(std::unique_ptr<ClientEntity> entity) {
  if (!entity || entity->Id() == kInvalidEntityId) {
    return false;
  }
  const EntityId id = entity->Id();
  return entities_.emplace(id, std::move(entity)).second;
}

bool ClientEntityManager::Remove(EntityId id) {
  return entities_.erase(id) > 0;
}

ClientEntity* ClientEntityManager::Find(EntityId id) const {
  const auto it = entities_.find(id);
  return it == entities_.end() ? nullptr : it->second.get();
}

void ClientEntityManager::Clear() {
  entities_.clear();
}

bool ClientEntityManager::HandleCreate(EntityId id, EntityTypeId type_id) {
  if (id == kInvalidEntityId || entities_.contains(id)) return false;
  const auto it = factories_.find(type_id);
  if (it == factories_.end()) return false;
  auto entity = it->second(id, type_id);
  if (!entity) return false;
  entities_.emplace(id, std::move(entity));
  return true;
}

bool ClientEntityManager::HandleEnter(EntityId id, EntityTypeId type_id, double server_time,
                                      const Vec3& pos, const Vec3& dir, bool on_ground) {
  if (id == kInvalidEntityId) return false;
  ClientEntity* raw = Find(id);
  if (raw == nullptr) {
    const auto it = factories_.find(type_id);
    if (it == factories_.end()) return false;
    auto entity = it->second(id, type_id);
    if (!entity) return false;
    raw = entity.get();
    entities_.emplace(id, std::move(entity));
  }
  raw->OnPositionReceived(server_time, pos, dir, on_ground);
  return true;
}

void ClientEntityManager::HandlePositionUpdate(EntityId id, double server_time, const Vec3& pos,
                                               const Vec3& dir, bool on_ground) {
  if (auto* entity = Find(id)) {
    entity->OnPositionReceived(server_time, pos, dir, on_ground);
  }
}

void ClientEntityManager::TickAll(double dt) {
  for (const auto& [id, entity] : entities_) {
    entity->TickInterpolation(dt);
  }
}

}  // namespace atlas
