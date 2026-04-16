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

// ============================================================================
// WatcherForwarder
//
// Routes WatcherRequest messages from clients / tools to the target process
// and relays the WatcherReply back to the original requester.
//
// Lifecycle:
//   1. handle_request()  — called when a WatcherRequest arrives on any channel.
//      Looks up the target in ProcessRegistry, sends a WatcherForward to it,
//      and records a pending entry.
//   2. handle_reply()    — called when a WatcherReply arrives from a process.
//      Matches by request_id and sends WatcherResponse back to the requester.
//   3. check_timeouts()  — call periodically (e.g. every second) to expire
//      pending requests that have not received a reply within kReplyTimeout.
// ============================================================================

class WatcherForwarder {
 public:
  static constexpr Duration kReplyTimeout = std::chrono::seconds(5);
  using ChannelResolver = std::function<Channel*(const Address&)>;

  // ProcessRegistry is used read-only to look up channels for target processes.
  WatcherForwarder(const ProcessRegistry& registry, ChannelResolver requester_resolver);

  // Forward a request to the target process.
  // requester_channel — the channel from which the WatcherRequest arrived.
  void HandleRequest(Channel* requester_channel, const WatcherRequest& req);

  // Called when a WatcherReply arrives from a target process.
  // source_channel — the channel from which the reply arrived.
  void HandleReply(Channel* source_channel, const WatcherReply& reply);

  // Expire timed-out pending requests (send an error WatcherResponse to the requester).
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

  // Monotonically increasing ID used when forwarding to target process.
  // Maps back to original requester via pending_ table.
  uint32_t next_forward_id_{1};
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_WATCHER_FORWARDER_H_
