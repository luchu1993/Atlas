#include "entity_manager.h"

#include "foundation/log.h"

namespace atlas {

auto EntityManager::AllocateId() -> EntityID {
  if (id_client_) return id_client_->AllocateId();

  ATLAS_LOG_WARNING("EntityManager: no IDClient installed, cannot allocate EntityID");
  return kInvalidEntityID;
}

auto EntityManager::Create(uint16_t type_id, bool has_client, DatabaseID dbid) -> BaseEntity* {
  if (dbid != kInvalidDBID) {
    auto existing = dbid_index_.find(dbid);
    if (existing != dbid_index_.end()) {
      ATLAS_LOG_ERROR(
          "EntityManager: cannot create entity type {} with duplicate DBID {} "
          "(already bound to entity {})",
          type_id, dbid, existing->second);
      return nullptr;
    }
  }

  EntityID id = AllocateId();
  if (id == kInvalidEntityID) {
    return nullptr;
  }

  std::unique_ptr<BaseEntity> ent;
  if (has_client) {
    ent = std::make_unique<Proxy>(id, type_id, dbid);
    ++proxy_count_;
  } else {
    ent = std::make_unique<BaseEntity>(id, type_id, dbid);
  }

  auto* ptr = ent.get();
  entities_.emplace(id, std::move(ent));
  if (dbid != kInvalidDBID) {
    dbid_index_[dbid] = id;
  }
  return ptr;
}

auto EntityManager::Find(EntityID id) const -> BaseEntity* {
  auto it = entities_.find(id);
  return it != entities_.end() ? it->second.get() : nullptr;
}

auto EntityManager::FindByDbid(DatabaseID dbid) const -> BaseEntity* {
  auto it = dbid_index_.find(dbid);
  if (it == dbid_index_.end()) {
    return nullptr;
  }
  return this->Find(it->second);
}

auto EntityManager::FindProxy(EntityID id) const -> Proxy* {
  return dynamic_cast<Proxy*>(Find(id));
}

auto EntityManager::FindProxyBySession(const SessionKey& session_key) -> Proxy* {
  auto it = session_index_.find(session_key);
  if (it != session_index_.end()) {
    return this->FindProxy(it->second);
  }

  auto rit = retired_sessions_.find(session_key);
  if (rit != retired_sessions_.end() && Clock::now() <= rit->second.expires_at) {
    const EntityID kEid = rit->second.entity_id;
    retired_sessions_.erase(rit);
    return this->FindProxy(kEid);
  }

  return nullptr;
}

auto EntityManager::AssignDbid(EntityID id, DatabaseID dbid) -> bool {
  auto* ent = this->Find(id);
  if (!ent) {
    return false;
  }

  const auto kOldDbid = ent->Dbid();
  if (kOldDbid == dbid) {
    return true;
  }

  if (kOldDbid != kInvalidDBID) {
    dbid_index_.erase(kOldDbid);
  }

  if (dbid != kInvalidDBID) {
    auto existing = dbid_index_.find(dbid);
    if (existing != dbid_index_.end() && existing->second != id) {
      ATLAS_LOG_ERROR("EntityManager: DBID {} is already bound to entity {}", dbid,
                      existing->second);
      if (kOldDbid != kInvalidDBID) {
        dbid_index_[kOldDbid] = id;
      }
      return false;
    }
    dbid_index_[dbid] = id;
  }

  ent->SetDbid(dbid);
  return true;
}

auto EntityManager::AssignSessionKey(EntityID id, const SessionKey& session_key) -> bool {
  auto* proxy = this->FindProxy(id);
  if (!proxy) {
    return false;
  }

  const auto kOldKey = proxy->GetSessionKey();
  if (kOldKey == session_key) {
    return true;
  }

  if (!kOldKey.IsZero()) {
    session_index_.erase(kOldKey);
    if (!session_key.IsZero()) {
      retired_sessions_[kOldKey] = RetiredSession{id, Clock::now() + kRetiredSessionTtl};
    }
  }

  if (!session_key.IsZero()) {
    auto existing = session_index_.find(session_key);
    if (existing != session_index_.end() && existing->second != id) {
      ATLAS_LOG_ERROR("EntityManager: SessionKey already bound to entity {}", existing->second);
      if (!kOldKey.IsZero()) {
        session_index_[kOldKey] = id;
      }
      return false;
    }
    session_index_[session_key] = id;
  }

  proxy->SetSessionKey(session_key);
  return true;
}

auto EntityManager::ClearSessionKey(EntityID id) -> bool {
  return this->AssignSessionKey(id, SessionKey{});
}

void EntityManager::Destroy(EntityID id) {
  auto it = entities_.find(id);
  if (it == entities_.end()) {
    return;
  }
  this->EraseIndexesFor(*it->second);
  entities_.erase(it);
}

auto EntityManager::IsRangeLow() const -> bool {
  if (id_client_) return id_client_->NeedsRefill();

  return true;  // no IDClient - always request more
}

void EntityManager::FlushDestroyed() {
  for (auto it = entities_.begin(); it != entities_.end();) {
    if (it->second->IsPendingDestroy()) {
      this->EraseIndexesFor(*it->second);
      it = entities_.erase(it);
    } else
      ++it;
  }
}

void EntityManager::CleanupRetiredSessions() {
  const auto kNow = Clock::now();
  for (auto it = retired_sessions_.begin(); it != retired_sessions_.end();) {
    if (kNow > it->second.expires_at)
      it = retired_sessions_.erase(it);
    else
      ++it;
  }
}

void EntityManager::EraseIndexesFor(const BaseEntity& ent) {
  if (ent.Dbid() != kInvalidDBID) {
    auto it = dbid_index_.find(ent.Dbid());
    if (it != dbid_index_.end() && it->second == ent.EntityId()) {
      dbid_index_.erase(it);
    }
  }

  if (const auto* proxy = dynamic_cast<const Proxy*>(&ent)) {
    if (proxy_count_ > 0) {
      --proxy_count_;
    }
    if (!proxy->GetSessionKey().IsZero()) {
      auto it = session_index_.find(proxy->GetSessionKey());
      if (it != session_index_.end() && it->second == ent.EntityId()) {
        session_index_.erase(it);
      }
    }
  }
}

}  // namespace atlas
