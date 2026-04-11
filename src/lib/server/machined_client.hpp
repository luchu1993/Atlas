#pragma once

#include "foundation/time.hpp"
#include "network/address.hpp"
#include "network/machined_types.hpp"
#include "server/server_config.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace atlas
{

class Channel;
class EventDispatcher;
class NetworkInterface;

// ============================================================================
// MachinedClient
//
// Provides a server process with the ability to:
//   - Connect to machined via TCP.
//   - Register / deregister itself.
//   - Send periodic heartbeats.
//   - Query registered processes (synchronous via process_once loop, or async).
//   - Subscribe to Birth/Death notifications.
//   - Forward WatcherRequest to machined on behalf of the local WatcherRegistry.
//
// Thread safety: all methods must be called from the EventDispatcher thread.
// ============================================================================

class MachinedClient
{
public:
    using BirthCallback = std::function<void(const machined::BirthNotification&)>;
    using DeathCallback = std::function<void(const machined::DeathNotification&)>;

    MachinedClient(EventDispatcher& dispatcher, NetworkInterface& network);
    ~MachinedClient();

    // Non-copyable, non-movable
    MachinedClient(const MachinedClient&) = delete;
    MachinedClient& operator=(const MachinedClient&) = delete;

    // ---- Connection ---------------------------------------------------------

    // Connect to machined.  Returns false if the connect call failed immediately.
    // The connection may still be pending (WouldBlock) — use is_connected() to check.
    [[nodiscard]] auto connect(const Address& machined_addr) -> bool;

    [[nodiscard]] auto is_connected() const -> bool;

    // ---- Registration -------------------------------------------------------

    // Register this process with machined.
    // Should be called after connect() succeeds.
    void send_register(const ServerConfig& cfg);

    // Deregister gracefully (called from fini()).
    void send_deregister(const ServerConfig& cfg);

    // ---- Heartbeats ---------------------------------------------------------

    void send_heartbeat(float load = 0.0f, uint32_t entity_count = 0);

    // ---- Query (synchronous — safe during init()) ---------------------------

    // Blocks the event loop (up to timeout) waiting for a QueryResponse.
    // Returns empty vector on timeout.
    [[nodiscard]] auto query_sync(ProcessType type, Duration timeout = std::chrono::seconds(5))
        -> std::vector<machined::ProcessInfo>;

    // ---- Query (async) ------------------------------------------------------

    using QueryCallback = std::function<void(std::vector<machined::ProcessInfo>)>;
    void query_async(ProcessType type, QueryCallback cb);

    // ---- Listener subscriptions ---------------------------------------------

    void subscribe(machined::ListenerType listener_type, ProcessType target_type,
                   BirthCallback on_birth, DeathCallback on_death);

    // ---- Callbacks called by MachinedClient itself ---------------------------

    // Called by ServerApp on each tick to send heartbeat at the right interval.
    void tick(float load = 0.0f, uint32_t entity_count = 0);

    static constexpr Duration kHeartbeatInterval = std::chrono::seconds(5);

private:
    // Register message handlers on NetworkInterface::interface_table
    void register_handlers();

    void on_register_ack(const Address& src, Channel* ch, const machined::RegisterAck& msg);
    void on_heartbeat_ack(const Address& src, Channel* ch, const machined::HeartbeatAck& msg);
    void on_query_response(const Address& src, Channel* ch, const machined::QueryResponse& msg);
    void on_birth_notification(const Address& src, Channel* ch,
                               const machined::BirthNotification& msg);
    void on_death_notification(const Address& src, Channel* ch,
                               const machined::DeathNotification& msg);
    void on_listener_ack(const Address& src, Channel* ch, const machined::ListenerAck& msg);
    void on_watcher_response(const Address& src, Channel* ch, const machined::WatcherResponse& msg);
    void on_death_from_machined(const Address& src, Channel* ch,
                                const machined::DeathNotification& msg);

    EventDispatcher& dispatcher_;
    NetworkInterface& network_;
    Channel* channel_{nullptr};

    // Pending synchronous query
    struct SyncQuery
    {
        bool done{false};
        std::vector<machined::ProcessInfo> result;
    };
    std::optional<SyncQuery> sync_query_;

    // Pending async query callback
    QueryCallback async_query_cb_;

    // Birth/Death subscribers
    struct Subscription
    {
        machined::ListenerType listener_type;
        ProcessType target_type;
        BirthCallback on_birth;
        DeathCallback on_death;
    };
    std::vector<Subscription> subscriptions_;

    // Heartbeat tracking
    TimePoint last_heartbeat_{};
    bool registered_{false};

    // When machined advertises a UDP heartbeat port, we send heartbeats there
    // via a plain UdpChannel instead of the TCP control channel.
    Address machined_heartbeat_udp_addr_;  // {0,0} = not set, use TCP

    bool handlers_registered_{false};
};

}  // namespace atlas
