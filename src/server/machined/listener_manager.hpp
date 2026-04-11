#pragma once

#include "machined/process_registry.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"
#include "server/server_config.hpp"

#include <vector>

namespace atlas::machined
{

// ============================================================================
// ListenerManager
//
// Tracks channels that have subscribed to Birth/Death events for a specific
// ProcessType.  When a process is registered or unregistered, MachinedApp
// calls notify_birth() / notify_death() to fan-out the notification to all
// matching listeners.
// ============================================================================

class ListenerManager
{
public:
    struct Subscription
    {
        Channel* channel{nullptr};
        ListenerType listener_type{ListenerType::Both};
        ProcessType target_type{ProcessType::BaseApp};
    };

    // Add a subscription.  Duplicate (channel, target_type) pairs are replaced.
    void add_listener(Channel* channel, ListenerType listener_type, ProcessType target_type);

    // Remove all subscriptions for a given channel (called on disconnect).
    void remove_all(Channel* channel);

    // Send BirthNotification to matching subscribers.
    // Skips dead channels (nullptr or not connected).
    void notify_birth(const BirthNotification& notif);

    // Send DeathNotification to matching subscribers.
    void notify_death(const DeathNotification& notif);

    [[nodiscard]] auto subscription_count() const -> std::size_t { return subs_.size(); }

private:
    std::vector<Subscription> subs_;
};

}  // namespace atlas::machined
