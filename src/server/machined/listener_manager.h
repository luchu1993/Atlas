#ifndef ATLAS_SERVER_MACHINED_LISTENER_MANAGER_H_
#define ATLAS_SERVER_MACHINED_LISTENER_MANAGER_H_

#include <vector>

#include "machined/process_registry.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "server/server_config.h"

namespace atlas::machined {

// ============================================================================
// ListenerManager
//
// Tracks channels that have subscribed to Birth/Death events for a specific
// ProcessType.  When a process is registered or unregistered, MachinedApp
// calls notify_birth() / notify_death() to fan-out the notification to all
// matching listeners.
// ============================================================================

class ListenerManager {
 public:
  struct Subscription {
    Channel* channel{nullptr};
    ListenerType listener_type{ListenerType::kBoth};
    ProcessType target_type{ProcessType::kBaseApp};
  };

  // Add a subscription.  Duplicate (channel, target_type) pairs are replaced.
  void AddListener(Channel* channel, ListenerType listener_type, ProcessType target_type);

  // Remove all subscriptions for a given channel (called on disconnect).
  void RemoveAll(Channel* channel);

  // Send BirthNotification to matching subscribers.
  // Skips dead channels (nullptr or not connected).
  void NotifyBirth(const BirthNotification& notif);

  // Send DeathNotification to matching subscribers.
  void NotifyDeath(const DeathNotification& notif);

  [[nodiscard]] auto SubscriptionCount() const -> std::size_t { return subs_.size(); }

 private:
  std::vector<Subscription> subs_;
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_LISTENER_MANAGER_H_
