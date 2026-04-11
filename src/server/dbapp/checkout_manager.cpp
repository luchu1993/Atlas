#include "checkout_manager.hpp"

namespace atlas
{

auto CheckoutManager::try_checkout(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner)
    -> CheckoutResult
{
    Key key{dbid, type_id};
    auto it = entries_.find(key);
    if (it != entries_.end())
    {
        if (it->second.state == State::Confirmed)
            return {CheckoutStatus::AlreadyCheckedOut, it->second.owner};
        return {CheckoutStatus::PendingCheckout, it->second.owner};
    }

    entries_[key] = Entry{new_owner, State::Checking};
    return {CheckoutStatus::Success, {}};
}

void CheckoutManager::confirm_checkout(DatabaseID dbid, uint16_t type_id)
{
    auto it = entries_.find({dbid, type_id});
    if (it != entries_.end())
        it->second.state = State::Confirmed;
}

void CheckoutManager::release_checkout(DatabaseID dbid, uint16_t type_id)
{
    entries_.erase({dbid, type_id});
}

void CheckoutManager::checkin(DatabaseID dbid, uint16_t type_id)
{
    entries_.erase({dbid, type_id});
}

auto CheckoutManager::get_owner(DatabaseID dbid, uint16_t type_id) const
    -> std::optional<CheckoutInfo>
{
    auto it = entries_.find({dbid, type_id});
    if (it == entries_.end())
        return std::nullopt;
    return it->second.owner;
}

auto CheckoutManager::clear_all_for(const Address& base_addr) -> int
{
    int count = 0;
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (it->second.owner.base_addr == base_addr)
        {
            it = entries_.erase(it);
            ++count;
        }
        else
        {
            ++it;
        }
    }
    return count;
}

}  // namespace atlas
