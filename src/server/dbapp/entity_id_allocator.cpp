#include "entity_id_allocator.hpp"

namespace atlas
{

EntityIdAllocator::EntityIdAllocator(IDatabase& db) : db_(db) {}

void EntityIdAllocator::startup(std::function<void(bool)> callback)
{
    db_.load_entity_id_counter(
        [this, cb = std::move(callback)](EntityID next_id)
        {
            next_id_ = (next_id > 0) ? next_id : 1;
            persisted_up_to_ = next_id_;
            ATLAS_LOG_INFO("EntityIdAllocator: loaded counter, next_id={}", next_id_);

            // Immediately persist with safety buffer so that if we crash
            // before the first explicit persist, recovery skips the buffer.
            persist(
                [cb](bool ok)
                {
                    if (cb)
                        cb(ok);
                });
        });
}

auto EntityIdAllocator::allocate(uint32_t count) -> std::pair<EntityID, EntityID>
{
    if (count == 0)
    {
        return {kInvalidEntityID, kInvalidEntityID};
    }

    EntityID start = next_id_;
    EntityID end = next_id_ + count - 1;
    next_id_ = end + 1;
    return {start, end};
}

void EntityIdAllocator::persist_if_needed(std::function<void(bool)> callback)
{
    if (next_id_ + kSafetyBuffer > persisted_up_to_)
    {
        persist(std::move(callback));
    }
    else if (callback)
    {
        callback(true);
    }
}

void EntityIdAllocator::persist(std::function<void(bool)> callback)
{
    EntityID target = next_id_ + kSafetyBuffer;
    db_.save_entity_id_counter(
        target,
        [this, target, cb = std::move(callback)](bool ok)
        {
            if (ok)
            {
                persisted_up_to_ = target;
                ATLAS_LOG_DEBUG("EntityIdAllocator: persisted up_to={}", persisted_up_to_);
            }
            else
            {
                ATLAS_LOG_ERROR("EntityIdAllocator: failed to persist counter");
            }
            if (cb)
                cb(ok);
        });
}

}  // namespace atlas
