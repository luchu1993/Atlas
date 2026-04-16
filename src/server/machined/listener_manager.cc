#include "machined/listener_manager.h"

#include <algorithm>

#include "foundation/log.h"

namespace atlas::machined {

void ListenerManager::AddListener(Channel* channel, ListenerType listener_type,
                                  ProcessType target_type) {
  // Replace if (channel, target_type) already exists
  for (auto& s : subs_) {
    if (s.channel == channel && s.target_type == target_type) {
      s.listener_type = listener_type;
      return;
    }
  }
  subs_.push_back({channel, listener_type, target_type});
}

void ListenerManager::RemoveAll(Channel* channel) {
  auto before = subs_.size();
  std::erase_if(subs_, [channel](const Subscription& s) { return s.channel == channel; });
  auto removed = before - subs_.size();
  if (removed > 0) {
    ATLAS_LOG_DEBUG("ListenerManager: removed {} subscription(s) for disconnected channel",
                    removed);
  }
}

void ListenerManager::NotifyBirth(const BirthNotification& notif) {
  for (const auto& s : subs_) {
    if (s.target_type != notif.process_type) continue;
    if (s.listener_type != ListenerType::kBirth && s.listener_type != ListenerType::kBoth) continue;
    if (s.channel == nullptr || !s.channel->IsConnected()) continue;

    if (auto r = s.channel->SendMessage(notif); !r) {
      ATLAS_LOG_WARNING("ListenerManager: failed to send BirthNotification: {}",
                        r.Error().Message());
    }
  }
}

void ListenerManager::NotifyDeath(const DeathNotification& notif) {
  for (const auto& s : subs_) {
    if (s.target_type != notif.process_type) continue;
    if (s.listener_type != ListenerType::kDeath && s.listener_type != ListenerType::kBoth) continue;
    if (s.channel == nullptr || !s.channel->IsConnected()) continue;

    if (auto r = s.channel->SendMessage(notif); !r) {
      ATLAS_LOG_WARNING("ListenerManager: failed to send DeathNotification: {}",
                        r.Error().Message());
    }
  }
}

}  // namespace atlas::machined
