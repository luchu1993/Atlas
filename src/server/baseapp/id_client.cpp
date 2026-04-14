#include "id_client.hpp"

#include "foundation/log.hpp"

namespace atlas
{

auto IDClient::allocate_id() -> EntityID
{
    if (total_available_ < kCriticallyLow)
    {
        ATLAS_LOG_WARNING("IDClient: critically low ({} IDs remaining)", total_available_);
        return kInvalidEntityID;
    }

    while (!ranges_.empty())
    {
        auto& front = ranges_.front();
        if (front.start <= front.end)
        {
            EntityID id = front.start++;
            --total_available_;
            if (front.start > front.end)
                ranges_.pop_front();
            return id;
        }
        ranges_.pop_front();
    }

    return kInvalidEntityID;
}

void IDClient::add_ids(EntityID start, EntityID end)
{
    if (start == kInvalidEntityID || end == kInvalidEntityID || start > end)
    {
        ATLAS_LOG_WARNING("IDClient: ignoring invalid range [{}, {}]", start, end);
        return;
    }

    uint64_t count = static_cast<uint64_t>(end) - start + 1;
    ranges_.push_back({start, end});
    total_available_ += count;
    ATLAS_LOG_DEBUG("IDClient: added range [{}, {}] ({} IDs), total_available={}", start, end,
                    count, total_available_);
}

auto IDClient::needs_refill() const -> bool
{
    return total_available_ < kLow;
}

auto IDClient::ids_to_request() const -> uint32_t
{
    if (total_available_ >= kHigh)
        return 0;

    return static_cast<uint32_t>(kDesired);
}

}  // namespace atlas
