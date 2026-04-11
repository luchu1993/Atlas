#include "machined/listener_manager.hpp"

#include "foundation/log.hpp"

#include <algorithm>

namespace atlas::machined
{

void ListenerManager::add_listener(Channel* channel, ListenerType listener_type,
                                   ProcessType target_type)
{
    // Replace if (channel, target_type) already exists
    for (auto& s : subs_)
    {
        if (s.channel == channel && s.target_type == target_type)
        {
            s.listener_type = listener_type;
            return;
        }
    }
    subs_.push_back({channel, listener_type, target_type});
}

void ListenerManager::remove_all(Channel* channel)
{
    auto before = subs_.size();
    std::erase_if(subs_, [channel](const Subscription& s) { return s.channel == channel; });
    auto removed = before - subs_.size();
    if (removed > 0)
    {
        ATLAS_LOG_DEBUG("ListenerManager: removed {} subscription(s) for disconnected channel",
                        removed);
    }
}

void ListenerManager::notify_birth(const BirthNotification& notif)
{
    for (const auto& s : subs_)
    {
        if (s.target_type != notif.process_type)
            continue;
        if (s.listener_type != ListenerType::Birth && s.listener_type != ListenerType::Both)
            continue;
        if (s.channel == nullptr || !s.channel->is_connected())
            continue;

        if (auto r = s.channel->send_message(notif); !r)
        {
            ATLAS_LOG_WARNING("ListenerManager: failed to send BirthNotification: {}",
                              r.error().message());
        }
    }
}

void ListenerManager::notify_death(const DeathNotification& notif)
{
    for (const auto& s : subs_)
    {
        if (s.target_type != notif.process_type)
            continue;
        if (s.listener_type != ListenerType::Death && s.listener_type != ListenerType::Both)
            continue;
        if (s.channel == nullptr || !s.channel->is_connected())
            continue;

        if (auto r = s.channel->send_message(notif); !r)
        {
            ATLAS_LOG_WARNING("ListenerManager: failed to send DeathNotification: {}",
                              r.error().message());
        }
    }
}

}  // namespace atlas::machined
