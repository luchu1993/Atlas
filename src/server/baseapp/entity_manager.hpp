#pragma once

#include "base_entity.hpp"
#include "foundation/time.hpp"
#include "id_client.hpp"
#include "server/entity_types.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace atlas
{

// ============================================================================
// EntityManager — owns all BaseEntity instances on this BaseApp
//
// EntityID allocation is delegated to an IDClient which obtains IDs from
// DBApp via water-level-controlled batch requests.
// ============================================================================

class EntityManager
{
public:
    EntityManager() = default;

    // Install the IDClient for water-level ID allocation from DBApp.
    void set_id_client(IDClient* client) { id_client_ = client; }

    // Allocate a fresh EntityID from the IDClient cache.
    // Returns kInvalidEntityID when no IDClient is installed or cache is empty.
    [[nodiscard]] auto allocate_id() -> EntityID;

    // Returns true when IDs are running low and more should be requested.
    [[nodiscard]] auto is_range_low() const -> bool;

    // Create a new BaseEntity or Proxy and take ownership
    auto create(uint16_t type_id, bool has_client, DatabaseID dbid = kInvalidDBID) -> BaseEntity*;

    // Retrieve entity by ID (nullptr if not found)
    [[nodiscard]] auto find(EntityID id) const -> BaseEntity*;

    // Retrieve entity by DBID (nullptr if not found or not yet persisted)
    [[nodiscard]] auto find_by_dbid(DatabaseID dbid) const -> BaseEntity*;

    // Retrieve Proxy by ID (nullptr if not a Proxy or not found)
    [[nodiscard]] auto find_proxy(EntityID id) const -> Proxy*;

    // Retrieve Proxy by login session key (nullptr if not found).
    // Also checks recently-retired session keys to tolerate short-lived
    // overlap between session rotation and client authentication.
    [[nodiscard]] auto find_proxy_by_session(const SessionKey& session_key) -> Proxy*;

    // Remove expired entries from the retired-session table.
    void cleanup_retired_sessions();

    // Update secondary indexes when identity/session state changes.
    auto assign_dbid(EntityID id, DatabaseID dbid) -> bool;
    auto assign_session_key(EntityID id, const SessionKey& session_key) -> bool;
    auto clear_session_key(EntityID id) -> bool;

    // Destroy entity and remove from map
    void destroy(EntityID id);

    // Remove entities flagged pending_destroy
    void flush_destroyed();

    [[nodiscard]] auto size() const -> std::size_t { return entities_.size(); }
    [[nodiscard]] auto proxy_count() const -> std::size_t { return proxy_count_; }

    // Iterate all entities (read-only view of pointers)
    template <typename Fn>
    void for_each(Fn&& fn) const
    {
        for (auto& [id, ent] : entities_)
            fn(*ent);
    }

private:
    void erase_indexes_for(const BaseEntity& ent);

    IDClient* id_client_{nullptr};
    std::unordered_map<EntityID, std::unique_ptr<BaseEntity>> entities_;
    std::unordered_map<DatabaseID, EntityID> dbid_index_;
    std::unordered_map<SessionKey, EntityID> session_index_;
    struct RetiredSession
    {
        EntityID entity_id{kInvalidEntityID};
        TimePoint expires_at{};
    };
    std::unordered_map<SessionKey, RetiredSession> retired_sessions_;
    static constexpr Duration kRetiredSessionTtl = std::chrono::seconds(5);
    std::size_t proxy_count_{0};
};

}  // namespace atlas
