#include "entity_manager.hpp"

#include "foundation/log.hpp"

#include <algorithm>

namespace atlas
{

EntityManager::EntityManager(uint32_t app_index)
    : next_id_(app_index * kIdBucketSize + 1), range_start_(next_id_)
{
}

auto EntityManager::allocate_id() -> EntityID
{
    if (next_id_ > range_end_)
    {
        ATLAS_LOG_WARNING("EntityManager: EntityID range exhausted [{}..{}]", range_start_,
                          range_end_);
        return kInvalidEntityID;
    }

    EntityID id = next_id_++;
    if (next_id_ == 0)
        next_id_ = 1;  // wrap-around safety (shouldn't happen in practice)
    return id;
}

auto EntityManager::create(uint16_t type_id, bool has_client, DatabaseID dbid) -> BaseEntity*
{
    if (dbid != kInvalidDBID)
    {
        auto existing = dbid_index_.find(dbid);
        if (existing != dbid_index_.end())
        {
            ATLAS_LOG_ERROR(
                "EntityManager: cannot create entity type {} with duplicate DBID {} "
                "(already bound to entity {})",
                type_id, dbid, existing->second);
            return nullptr;
        }
    }

    EntityID id = allocate_id();
    if (id == kInvalidEntityID)
    {
        return nullptr;
    }

    std::unique_ptr<BaseEntity> ent;
    if (has_client)
    {
        ent = std::make_unique<Proxy>(id, type_id, dbid);
        ++proxy_count_;
    }
    else
    {
        ent = std::make_unique<BaseEntity>(id, type_id, dbid);
    }

    auto* ptr = ent.get();
    entities_.emplace(id, std::move(ent));
    if (dbid != kInvalidDBID)
    {
        dbid_index_[dbid] = id;
    }
    return ptr;
}

auto EntityManager::find(EntityID id) const -> BaseEntity*
{
    auto it = entities_.find(id);
    return it != entities_.end() ? it->second.get() : nullptr;
}

auto EntityManager::find_by_dbid(DatabaseID dbid) const -> BaseEntity*
{
    auto it = dbid_index_.find(dbid);
    if (it == dbid_index_.end())
    {
        return nullptr;
    }
    return this->find(it->second);
}

auto EntityManager::find_proxy(EntityID id) const -> Proxy*
{
    return dynamic_cast<Proxy*>(find(id));
}

auto EntityManager::find_proxy_by_session(const SessionKey& session_key) const -> Proxy*
{
    auto it = session_index_.find(session_key);
    if (it == session_index_.end())
    {
        return nullptr;
    }
    return this->find_proxy(it->second);
}

auto EntityManager::assign_dbid(EntityID id, DatabaseID dbid) -> bool
{
    auto* ent = this->find(id);
    if (!ent)
    {
        return false;
    }

    const auto old_dbid = ent->dbid();
    if (old_dbid == dbid)
    {
        return true;
    }

    if (old_dbid != kInvalidDBID)
    {
        dbid_index_.erase(old_dbid);
    }

    if (dbid != kInvalidDBID)
    {
        auto existing = dbid_index_.find(dbid);
        if (existing != dbid_index_.end() && existing->second != id)
        {
            ATLAS_LOG_ERROR("EntityManager: DBID {} is already bound to entity {}", dbid,
                            existing->second);
            if (old_dbid != kInvalidDBID)
            {
                dbid_index_[old_dbid] = id;
            }
            return false;
        }
        dbid_index_[dbid] = id;
    }

    ent->set_dbid(dbid);
    return true;
}

auto EntityManager::assign_session_key(EntityID id, const SessionKey& session_key) -> bool
{
    auto* proxy = this->find_proxy(id);
    if (!proxy)
    {
        return false;
    }

    const auto old_key = proxy->session_key();
    if (old_key == session_key)
    {
        return true;
    }

    if (!old_key.is_zero())
    {
        session_index_.erase(old_key);
    }

    if (!session_key.is_zero())
    {
        auto existing = session_index_.find(session_key);
        if (existing != session_index_.end() && existing->second != id)
        {
            ATLAS_LOG_ERROR("EntityManager: SessionKey already bound to entity {}",
                            existing->second);
            if (!old_key.is_zero())
            {
                session_index_[old_key] = id;
            }
            return false;
        }
        session_index_[session_key] = id;
    }

    proxy->set_session_key(session_key);
    return true;
}

auto EntityManager::clear_session_key(EntityID id) -> bool
{
    return this->assign_session_key(id, SessionKey{});
}

void EntityManager::destroy(EntityID id)
{
    auto it = entities_.find(id);
    if (it == entities_.end())
    {
        return;
    }
    this->erase_indexes_for(*it->second);
    entities_.erase(it);
}

void EntityManager::set_id_range(EntityID start, EntityID end)
{
    range_start_ = start;
    next_id_ = start;
    range_end_ = end;
}

void EntityManager::extend_id_range(EntityID new_end)
{
    range_end_ = std::max(new_end, range_end_);
}

auto EntityManager::is_range_low() const -> bool
{
    if (range_end_ == std::numeric_limits<EntityID>::max())
        return false;  // unbounded local allocation

    const auto remaining = this->range_remaining();
    const auto total_capacity =
        (range_end_ >= range_start_) ? static_cast<uint32_t>(range_end_ - range_start_ + 1) : 0u;
    const auto low_watermark = std::max<uint32_t>(1u, total_capacity / 5u);

    return remaining < low_watermark;
}

void EntityManager::flush_destroyed()
{
    for (auto it = entities_.begin(); it != entities_.end();)
    {
        if (it->second->is_pending_destroy())
        {
            this->erase_indexes_for(*it->second);
            it = entities_.erase(it);
        }
        else
            ++it;
    }
}

void EntityManager::erase_indexes_for(const BaseEntity& ent)
{
    if (ent.dbid() != kInvalidDBID)
    {
        auto it = dbid_index_.find(ent.dbid());
        if (it != dbid_index_.end() && it->second == ent.entity_id())
        {
            dbid_index_.erase(it);
        }
    }

    if (const auto* proxy = dynamic_cast<const Proxy*>(&ent))
    {
        if (proxy_count_ > 0)
        {
            --proxy_count_;
        }
        if (!proxy->session_key().is_zero())
        {
            auto it = session_index_.find(proxy->session_key());
            if (it != session_index_.end() && it->second == ent.entity_id())
            {
                session_index_.erase(it);
            }
        }
    }
}

}  // namespace atlas
