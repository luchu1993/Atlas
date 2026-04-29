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

// Owns all BaseEntity instances on this BaseApp; delegates EntityID
// allocation to an IDClient backed by DBApp water-level batch requests.
class EntityManager {
 public:
  EntityManager() = default;

  void SetIdClient(IDClient* client) { id_client_ = client; }

  // Returns kInvalidEntityID when no IDClient is installed or cache empty.
  [[nodiscard]] auto AllocateId() -> EntityID;

  [[nodiscard]] auto IsRangeLow() const -> bool;

  auto Create(uint16_t type_id, bool has_client, DatabaseID dbid = kInvalidDBID) -> BaseEntity*;

  [[nodiscard]] auto Find(EntityID id) const -> BaseEntity*;
  [[nodiscard]] auto FindByDbid(DatabaseID dbid) const -> BaseEntity*;
  [[nodiscard]] auto FindProxy(EntityID id) const -> Proxy*;

  // Also checks recently-retired keys to tolerate short overlap between
  // session rotation and client authentication.
  [[nodiscard]] auto FindProxyBySession(const SessionKey& session_key) -> Proxy*;

  void CleanupRetiredSessions();

  auto AssignDbid(EntityID id, DatabaseID dbid) -> bool;
  auto AssignSessionKey(EntityID id, const SessionKey& session_key) -> bool;
  auto ClearSessionKey(EntityID id) -> bool;

  void Destroy(EntityID id);
  void FlushDestroyed();

  [[nodiscard]] auto Size() const -> std::size_t { return entities_.size(); }
  [[nodiscard]] auto ProxyCount() const -> std::size_t { return proxy_count_; }

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
