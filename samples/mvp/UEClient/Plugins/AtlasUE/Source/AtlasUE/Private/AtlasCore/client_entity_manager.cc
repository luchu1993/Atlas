#include "AtlasCore/client_entity_manager.h"

namespace atlas {

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

}  // namespace atlas
