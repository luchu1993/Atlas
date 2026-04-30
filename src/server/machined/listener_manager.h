#ifndef ATLAS_SERVER_MACHINED_LISTENER_MANAGER_H_
#define ATLAS_SERVER_MACHINED_LISTENER_MANAGER_H_

#include <vector>

#include "machined/process_registry.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "server/server_config.h"

namespace atlas::machined {

class ListenerManager {
 public:
  struct Subscription {
    Channel* channel{nullptr};
    ListenerType listener_type{ListenerType::kBoth};
    ProcessType target_type{ProcessType::kBaseApp};
  };

  void AddListener(Channel* channel, ListenerType listener_type, ProcessType target_type);

  void RemoveAll(Channel* channel);

  void NotifyBirth(const BirthNotification& notif);

  void NotifyDeath(const DeathNotification& notif);

  [[nodiscard]] auto SubscriptionCount() const -> std::size_t { return subs_.size(); }

 private:
  std::vector<Subscription> subs_;
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_LISTENER_MANAGER_H_
