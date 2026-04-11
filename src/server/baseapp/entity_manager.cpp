#include "entity_manager.hpp"

#include "foundation/log.hpp"

#include <algorithm>

namespace atlas
{

EntityManager::EntityManager(uint32_t app_index) : next_id_(app_index * kIdBucketSize + 1) {}

auto EntityManager::allocate_id() -> EntityID
{
    EntityID id = next_id_++;
    if (next_id_ == 0)
        next_id_ = 1;  // wrap-around safety (shouldn't happen in practice)
    return id;
}

auto EntityManager::create(uint16_t type_id, bool has_client, DatabaseID dbid) -> BaseEntity*
{
    EntityID id = allocate_id();
    std::unique_ptr<BaseEntity> ent;
    if (has_client)
        ent = std::make_unique<Proxy>(id, type_id, dbid);
    else
        ent = std::make_unique<BaseEntity>(id, type_id, dbid);

    auto* ptr = ent.get();
    entities_.emplace(id, std::move(ent));
    return ptr;
}

auto EntityManager::find(EntityID id) const -> BaseEntity*
{
    auto it = entities_.find(id);
    return it != entities_.end() ? it->second.get() : nullptr;
}

auto EntityManager::find_proxy(EntityID id) const -> Proxy*
{
    return dynamic_cast<Proxy*>(find(id));
}

void EntityManager::destroy(EntityID id)
{
    entities_.erase(id);
}

void EntityManager::set_id_range(EntityID start, EntityID end)
{
    next_id_ = start;
    range_end_ = end;
}

void EntityManager::extend_id_range(EntityID new_end)
{
    range_end_ = new_end;
}

auto EntityManager::is_range_low() const -> bool
{
    if (range_end_ == std::numeric_limits<EntityID>::max())
        return false;  // unbounded local allocation
    uint32_t total = (range_end_ >= next_id_) ? (range_end_ - next_id_ + 1) : 0;
    uint32_t used = next_id_ - 1;
    (void)used;
    return total < (EntityManager::kIdBucketSize / 5);  // < 20%
}

void EntityManager::flush_destroyed()
{
    for (auto it = entities_.begin(); it != entities_.end();)
    {
        if (it->second->is_pending_destroy())
            it = entities_.erase(it);
        else
            ++it;
    }
}

}  // namespace atlas
