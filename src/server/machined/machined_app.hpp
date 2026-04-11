#pragma once

#include "machined/listener_manager.hpp"
#include "machined/process_registry.hpp"
#include "machined/watcher_forwarder.hpp"
#include "network/frequent_task.hpp"
#include "server/manager_app.hpp"

namespace atlas::machined
{

// ============================================================================
// MachinedApp — the machined process main application class
//
// Responsibilities:
//   - Accept TCP connections from other server processes.
//   - Handle Register / Deregister / Heartbeat / Query messages.
//   - Manage birth/death subscriptions via ListenerManager.
//   - Forward WatcherRequest to target processes via WatcherForwarder.
//   - Send graceful-shutdown notification to registered processes on exit.
//   - Expose runtime stats via WatcherRegistry.
// ============================================================================

class MachinedApp : public ManagerApp
{
public:
    MachinedApp(EventDispatcher& dispatcher, NetworkInterface& network);

    // Entry point for the machined process (delegates to run_app)
    static auto run(int argc, char* argv[]) -> int;

protected:
    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;
    void fini() override;

    void register_watchers() override;

    void on_tick_complete() override;

private:
    // Message handlers — called from interface_table dispatch
    void on_register(const Address& src, Channel* ch, const RegisterMessage& msg);
    void on_deregister(const Address& src, Channel* ch, const DeregisterMessage& msg);
    void on_heartbeat(const Address& src, Channel* ch, const HeartbeatMessage& msg);
    void on_query(const Address& src, Channel* ch, const QueryMessage& msg);
    void on_listener_register(const Address& src, Channel* ch, const ListenerRegister& msg);
    void on_watcher_request(const Address& src, Channel* ch, const WatcherRequest& msg);
    void on_watcher_reply(const Address& src, Channel* ch, const WatcherReply& msg);

    // Called when a TCP connection is accepted
    void on_accept(Channel& ch);

    // Called when a channel disconnects
    void on_disconnect(Channel& ch);

    // Heartbeat timeout checker — runs each tick
    void check_heartbeat_timeouts();

    ProcessRegistry process_registry_;
    ListenerManager listener_manager_;
    WatcherForwarder watcher_forwarder_;

    static constexpr Duration kHeartbeatTimeout = std::chrono::seconds(15);

    struct HeartbeatEntry
    {
        Channel* channel{nullptr};
        TimePoint last_heartbeat;
    };
    std::vector<HeartbeatEntry> heartbeat_entries_;

    // UDP port on which heartbeat datagrams are received (0 = not started)
    uint16_t heartbeat_udp_port_{0};
};

}  // namespace atlas::machined
