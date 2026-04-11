#pragma once

#include "foundation/time.hpp"
#include "machined/process_registry.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace atlas::machined
{

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

class WatcherForwarder
{
public:
    static constexpr Duration kReplyTimeout = std::chrono::seconds(5);

    // ProcessRegistry is used read-only to look up channels for target processes.
    explicit WatcherForwarder(const ProcessRegistry& registry);

    // Forward a request to the target process.
    // requester_channel — the channel from which the WatcherRequest arrived.
    void handle_request(Channel* requester_channel, const WatcherRequest& req);

    // Called when a WatcherReply arrives from a target process.
    // source_channel — the channel from which the reply arrived.
    void handle_reply(Channel* source_channel, const WatcherReply& reply);

    // Expire timed-out pending requests (send an error WatcherResponse to the requester).
    void check_timeouts();

    [[nodiscard]] auto pending_count() const -> std::size_t { return pending_.size(); }

private:
    struct PendingEntry
    {
        uint32_t request_id{0};
        Channel* requester_channel{nullptr};
        std::string target_name;
        TimePoint issued_at;
    };

    const ProcessRegistry& registry_;
    std::vector<PendingEntry> pending_;

    // Monotonically increasing ID used when forwarding to target process.
    // Maps back to original requester via pending_ table.
    uint32_t next_forward_id_{1};
};

}  // namespace atlas::machined
