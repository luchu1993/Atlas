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
//   kInvalidEntityID (0) is never returned.
// ============================================================================

class EntityManager
{
public:
    static constexpr uint32_t kIdBucketSize = 1'000'000u;

    // app_index  — zero-based index of this BaseApp in the cluster
    explicit EntityManager(uint32_t app_index = 0);

    // Allocate a fresh EntityID (never reused within one process lifetime)
    [[nodiscard]] auto allocate_id() -> EntityID;

    // Set the allocated ID range from BaseAppMgr (replaces the local bucket)
    void set_id_range(EntityID start, EntityID end);

    // Extend the upper bound of the current range
    void extend_id_range(EntityID new_end);

    // Returns true when < 20% of current range remains
    [[nodiscard]] auto is_range_low() const -> bool;

    [[nodiscard]] auto range_remaining() const -> uint32_t
    {
        return (next_id_ <= range_end_) ? (range_end_ - next_id_ + 1) : 0;
    }

    // Create a new BaseEntity or Proxy and take ownership
    auto create(uint16_t type_id, bool has_client, DatabaseID dbid = kInvalidDBID) -> BaseEntity*;

    // Retrieve entity by ID (nullptr if not found)
    [[nodiscard]] auto find(EntityID id) const -> BaseEntity*;

    // Retrieve Proxy by ID (nullptr if not a Proxy or not found)
    [[nodiscard]] auto find_proxy(EntityID id) const -> Proxy*;

    // Destroy entity and remove from map
    void destroy(EntityID id);

    // Remove entities flagged pending_destroy
    void flush_destroyed();

    [[nodiscard]] auto size() const -> std::size_t { return entities_.size(); }

    // Iterate all entities (read-only view of pointers)
    template <typename Fn>
    void for_each(Fn&& fn) const
    {
        for (auto& [id, ent] : entities_)
            fn(*ent);
    }

private:
    EntityID next_id_;
    EntityID range_end_{std::numeric_limits<EntityID>::max()};
    std::unordered_map<EntityID, std::unique_ptr<BaseEntity>> entities_;
};

}  // namespace atlas
