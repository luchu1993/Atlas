#ifndef ATLAS_SERVER_BASEAPP_ENTITY_MANAGER_H_
#define ATLAS_SERVER_BASEAPP_ENTITY_MANAGER_H_

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "base_entity.h"
#include "foundation/clock.h"
#include "id_client.h"
#include "server/entity_types.h"

namespace atlas {

// ============================================================================
// EntityManager — owns all BaseEntity instances on this BaseApp
//
// EntityID allocation is delegated to an IDClient which obtains IDs from
// DBApp via water-level-controlled batch requests.
// ============================================================================

class EntityManager {
 public:
  EntityManager() = default;

  // Install the IDClient for water-level ID allocation from DBApp.
  void SetIdClient(IDClient* client) { id_client_ = client; }

  // Allocate a fresh EntityID from the IDClient cache.
  // Returns kInvalidEntityID when no IDClient is installed or cache is empty.
  [[nodiscard]] auto AllocateId() -> EntityID;

  // Returns true when IDs are running low and more should be requested.
  [[nodiscard]] auto IsRangeLow() const -> bool;

  // Create a new BaseEntity or Proxy and take ownership
  auto Create(uint16_t type_id, bool has_client, DatabaseID dbid = kInvalidDBID) -> BaseEntity*;

  // Retrieve entity by ID (nullptr if not found)
  [[nodiscard]] auto Find(EntityID id) const -> BaseEntity*;

  // Retrieve entity by DBID (nullptr if not found or not yet persisted)
  [[nodiscard]] auto FindByDbid(DatabaseID dbid) const -> BaseEntity*;

  // Retrieve Proxy by ID (nullptr if not a Proxy or not found)
  [[nodiscard]] auto FindProxy(EntityID id) const -> Proxy*;

  // Retrieve Proxy by login session key (nullptr if not found).
  // Also checks recently-retired session keys to tolerate short-lived
  // overlap between session rotation and client authentication.
  [[nodiscard]] auto FindProxyBySession(const SessionKey& session_key) -> Proxy*;

  // Remove expired entries from the retired-session table.
  void CleanupRetiredSessions();

  // Update secondary indexes when identity/session state changes.
  auto AssignDbid(EntityID id, DatabaseID dbid) -> bool;
  auto AssignSessionKey(EntityID id, const SessionKey& session_key) -> bool;
  auto ClearSessionKey(EntityID id) -> bool;

  // Destroy entity and remove from map
  void Destroy(EntityID id);

  // Remove entities flagged pending_destroy
  void FlushDestroyed();

  [[nodiscard]] auto Size() const -> std::size_t { return entities_.size(); }
  [[nodiscard]] auto ProxyCount() const -> std::size_t { return proxy_count_; }

  // Iterate all entities (read-only view of pointers)
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (auto& [id, ent] : entities_) fn(*ent);
  }

 private:
  void EraseIndexesFor(const BaseEntity& ent);

  IDClient* id_client_{nullptr};
  std::unordered_map<EntityID, std::unique_ptr<BaseEntity>> entities_;
  std::unordered_map<DatabaseID, EntityID> dbid_index_;
  std::unordered_map<SessionKey, EntityID> session_index_;
  struct RetiredSession {
    EntityID entity_id{kInvalidEntityID};
    TimePoint expires_at{};
  };
  std::unordered_map<SessionKey, RetiredSession> retired_sessions_;
  static constexpr Duration kRetiredSessionTtl = std::chrono::seconds(5);
  std::size_t proxy_count_{0};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_ENTITY_MANAGER_H_
