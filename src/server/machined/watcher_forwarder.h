#ifndef ATLAS_SERVER_MACHINED_WATCHER_FORWARDER_H_
#define ATLAS_SERVER_MACHINED_WATCHER_FORWARDER_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "foundation/clock.h"
#include "machined/process_registry.h"
#include "network/address.h"
#include "network/channel.h"
#include "network/machined_types.h"

namespace atlas::machined {

// Relays Watcher requests through machined while preserving requester state.
class WatcherForwarder {
 public:
  static constexpr Duration kReplyTimeout = std::chrono::seconds(5);
  using ChannelResolver = std::function<Channel*(const Address&)>;

  WatcherForwarder(const ProcessRegistry& registry, ChannelResolver requester_resolver);

  void HandleRequest(Channel* requester_channel, const WatcherRequest& req);

  void HandleReply(Channel* source_channel, const WatcherReply& reply);

  void CheckTimeouts();

  [[nodiscard]] auto PendingCount() const -> std::size_t { return pending_.size(); }

 private:
  struct PendingEntry {
    uint32_t forwarded_request_id{0};
    uint32_t requester_request_id{0};
    Address requester_addr;
    std::string target_name;
    TimePoint issued_at;
  };

  const ProcessRegistry& registry_;
  ChannelResolver requester_resolver_;
  std::vector<PendingEntry> pending_;

  uint32_t next_forward_id_{1};
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_WATCHER_FORWARDER_H_
