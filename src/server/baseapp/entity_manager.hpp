#pragma once

#include "base_entity.hpp"
#include "server/entity_types.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>

namespace atlas
{

// ============================================================================
// EntityManager — owns all BaseEntity instances on this BaseApp
//
// EntityID allocation:
//   IDs are allocated locally using a monotonically-increasing counter.
//   The counter starts at (app_index * kIdBucketSize + 1) so that each
//   BaseApp instance produces non-overlapping IDs without coordination.
//   When a manager-assigned range is installed, allocation is constrained to
//   [range_start_, range_end_]. If the range is exhausted, allocate_id()
//   returns kInvalidEntityID instead of spilling into another app's range.
// ============================================================================

class EntityManager
{
public:
    static constexpr uint32_t kIdBucketSize = 1'000'000u;

    // app_index  — zero-based index of this BaseApp in the cluster
    explicit EntityManager(uint32_t app_index = 0);

    // Allocate a fresh EntityID (never reused within one process lifetime).
    // Returns kInvalidEntityID when the current manager-assigned range is exhausted.
    [[nodiscard]] auto allocate_id() -> EntityID;

    // Set the allocated ID range from BaseAppMgr (replaces the local bucket)
    void set_id_range(EntityID start, EntityID end);

    // Extend the upper bound of the current range
    void extend_id_range(EntityID new_end);

    // Returns true when < 20% of the currently assigned range remains.
    [[nodiscard]] auto is_range_low() const -> bool;

    [[nodiscard]] auto range_remaining() const -> uint32_t
    {
        return (next_id_ <= range_end_) ? (range_end_ - next_id_ + 1) : 0;
    }

    // Create a new BaseEntity or Proxy and take ownership
    auto create(uint16_t type_id, bool has_client, DatabaseID dbid = kInvalidDBID) -> BaseEntity*;

    // Retrieve entity by ID (nullptr if not found)
    [[nodiscard]] auto find(EntityID id) const -> BaseEntity*;

    // Retrieve entity by DBID (nullptr if not found or not yet persisted)
    [[nodiscard]] auto find_by_dbid(DatabaseID dbid) const -> BaseEntity*;

    // Retrieve Proxy by ID (nullptr if not a Proxy or not found)
    [[nodiscard]] auto find_proxy(EntityID id) const -> Proxy*;

    // Retrieve Proxy by login session key (nullptr if not found)
    [[nodiscard]] auto find_proxy_by_session(const SessionKey& session_key) const -> Proxy*;

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

    EntityID next_id_;
    EntityID range_start_{1};
    EntityID range_end_{std::numeric_limits<EntityID>::max()};
    std::unordered_map<EntityID, std::unique_ptr<BaseEntity>> entities_;
    std::unordered_map<DatabaseID, EntityID> dbid_index_;
    std::unordered_map<SessionKey, EntityID> session_index_;
    std::size_t proxy_count_{0};
};

}  // namespace atlas
