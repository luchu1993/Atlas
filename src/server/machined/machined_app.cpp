#include "machined/machined_app.hpp"

#include "foundation/log.hpp"
#include "foundation/time.hpp"
#include "network/event_dispatcher.hpp"
#include "network/network_interface.hpp"
#include "server/server_config.hpp"

#include <algorithm>

namespace atlas::machined
{

MachinedApp::MachinedApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network), watcher_forwarder_(process_registry_)
{
}

auto MachinedApp::run(int argc, char* argv[]) -> int
{
    EventDispatcher dispatcher;
    NetworkInterface network(dispatcher);
    MachinedApp app(dispatcher, network);
    return app.run_app(argc, argv);
}

auto MachinedApp::init(int argc, char* argv[]) -> bool
{
    if (!ManagerApp::init(argc, argv))
        return false;

    auto& cfg = config();

    // Start TCP server on the machined internal port (for register/notify/query)
    Address listen_addr(0, cfg.internal_port);
    if (auto r = network().start_tcp_server(listen_addr); !r)
    {
        ATLAS_LOG_CRITICAL("MachinedApp: failed to start TCP server: {}", r.error().message());
        return false;
    }

    // Install accept callback so we can attach disconnect handlers per-connection
    network().set_accept_callback([this](Channel& ch) { on_accept(ch); });

    // Start UDP socket for heartbeat datagrams on (internal_port + 1).
    // This avoids the TCP ack round-trip for high-frequency heartbeats.
    uint16_t udp_port = (cfg.internal_port > 0) ? static_cast<uint16_t>(cfg.internal_port + 1) : 0;
    if (udp_port > 0)
    {
        Address udp_addr(0, udp_port);
        if (auto r = network().start_udp(udp_addr); !r)
        {
            ATLAS_LOG_WARNING("MachinedApp: failed to start UDP heartbeat socket on port {}: {}",
                              udp_port, r.error().message());
            // Non-fatal: fall back to TCP-only heartbeats
            udp_port = 0;
        }
        else
        {
            heartbeat_udp_port_ = network().udp_address().port();
            ATLAS_LOG_INFO("MachinedApp: UDP heartbeat listening on port {}", heartbeat_udp_port_);
        }
    }

    // Register message handlers
    auto& table = network().interface_table();

    (void)table.register_typed_handler<RegisterMessage>(
        [this](const Address& src, Channel* ch, const RegisterMessage& msg)
        { on_register(src, ch, msg); });

    (void)table.register_typed_handler<DeregisterMessage>(
        [this](const Address& src, Channel* ch, const DeregisterMessage& msg)
        { on_deregister(src, ch, msg); });

    (void)table.register_typed_handler<HeartbeatMessage>(
        [this](const Address& src, Channel* ch, const HeartbeatMessage& msg)
        { on_heartbeat(src, ch, msg); });

    (void)table.register_typed_handler<QueryMessage>(
        [this](const Address& src, Channel* ch, const QueryMessage& msg)
        { on_query(src, ch, msg); });

    (void)table.register_typed_handler<ListenerRegister>(
        [this](const Address& src, Channel* ch, const ListenerRegister& msg)
        { on_listener_register(src, ch, msg); });

    (void)table.register_typed_handler<WatcherRequest>(
        [this](const Address& src, Channel* ch, const WatcherRequest& msg)
        { on_watcher_request(src, ch, msg); });

    (void)table.register_typed_handler<WatcherReply>(
        [this](const Address& src, Channel* ch, const WatcherReply& msg)
        { on_watcher_reply(src, ch, msg); });

    ATLAS_LOG_INFO("MachinedApp: TCP listening on {}", network().tcp_address().to_string());
    return true;
}

void MachinedApp::fini()
{
    // Notify all registered processes that machined is shutting down
    DeathNotification shutdown_notif;
    shutdown_notif.process_type = ProcessType::Machined;
    shutdown_notif.name = "machined";
    shutdown_notif.reason = 0;

    process_registry_.for_each(
        [&](const ProcessEntry& entry)
        {
            if (entry.channel != nullptr && entry.channel->is_connected())
            {
                (void)entry.channel->send_message(shutdown_notif);
            }
        });

    ManagerApp::fini();
}

void MachinedApp::register_watchers()
{
    ManagerApp::register_watchers();

    auto& reg = watcher_registry();
    reg.add<std::string>("machined/registered_processes",
                         std::function<std::string()>{
                             [this]() { return std::to_string(process_registry_.size()); }});
    reg.add<std::string>(
        "machined/listener_subscriptions",
        std::function<std::string()>{
            [this]() { return std::to_string(listener_manager_.subscription_count()); }});
    reg.add<std::string>(
        "machined/watcher_pending",
        std::function<std::string()>{
            [this]() { return std::to_string(watcher_forwarder_.pending_count()); }});
}

void MachinedApp::on_tick_complete()
{
    check_heartbeat_timeouts();
    watcher_forwarder_.check_timeouts();
}

// ============================================================================
// Message handlers
// ============================================================================

void MachinedApp::on_register(const Address& /*src*/, Channel* ch, const RegisterMessage& msg)
{
    if (ch == nullptr)
        return;

    if (msg.protocol_version != kProtocolVersion)
    {
        RegisterAck ack;
        ack.success = false;
        ack.error_message = std::format("unsupported protocol version {}", msg.protocol_version);
        (void)ch->send_message(ack);
        return;
    }

    // Build ProcessEntry
    ProcessEntry entry;
    entry.process_type = msg.process_type;
    entry.name = msg.name;
    entry.internal_addr = Address(ch->remote_address().ip(), msg.internal_port);
    entry.external_addr = Address(ch->remote_address().ip(), msg.external_port);
    entry.pid = msg.pid;
    entry.channel = ch;

    RegisterAck ack;
    if (!process_registry_.register_process(entry))
    {
        ack.success = false;
        ack.error_message =
            std::format("duplicate name ({}, {})", static_cast<int>(msg.process_type), msg.name);
        (void)ch->send_message(ack);
        return;
    }

    // Track heartbeat — TCP fallback if UDP socket not available
    heartbeat_entries_.push_back({ch, Clock::now()});

    ack.success = true;
    ack.server_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
    ack.heartbeat_udp_port = heartbeat_udp_port_;
    (void)ch->send_message(ack);

    // Notify listeners
    BirthNotification notif;
    notif.process_type = entry.process_type;
    notif.name = entry.name;
    notif.internal_addr = entry.internal_addr;
    notif.external_addr = entry.external_addr;
    notif.pid = entry.pid;
    listener_manager_.notify_birth(notif);

    ATLAS_LOG_INFO("MachinedApp: registered process ({}, {})", static_cast<int>(msg.process_type),
                   msg.name);
}

void MachinedApp::on_deregister(const Address& /*src*/, Channel* ch, const DeregisterMessage& msg)
{
    if (ch == nullptr)
        return;

    auto removed = process_registry_.unregister_by_name(msg.process_type, msg.name);
    if (!removed)
    {
        ATLAS_LOG_WARNING("MachinedApp: deregister for unknown ({}, {})",
                          static_cast<int>(msg.process_type), msg.name);
        return;
    }

    // Remove heartbeat entry
    std::erase_if(heartbeat_entries_, [ch](const HeartbeatEntry& e) { return e.channel == ch; });
    listener_manager_.remove_all(ch);

    DeathNotification notif;
    notif.process_type = removed->process_type;
    notif.name = removed->name;
    notif.internal_addr = removed->internal_addr;
    notif.reason = 0;  // normal deregister
    listener_manager_.notify_death(notif);
}

void MachinedApp::on_heartbeat(const Address& src, Channel* ch, const HeartbeatMessage& msg)
{
    if (ch == nullptr)
        return;

    // UDP heartbeat: src.ip() matches the registered process IP, but the port
    // is the ephemeral send port — look up by TCP channel first, then by IP.
    Channel* tcp_ch = process_registry_.find_tcp_channel_by_ip(src.ip());
    Channel* registry_ch = (tcp_ch != nullptr) ? tcp_ch : ch;

    process_registry_.update_load(registry_ch, msg.load, msg.entity_count);

    for (auto& e : heartbeat_entries_)
    {
        if (e.channel == registry_ch)
        {
            e.last_heartbeat = Clock::now();
            break;
        }
    }

    // Only send HeartbeatAck on TCP channels; UDP heartbeats are fire-and-forget.
    if (tcp_ch == nullptr)
    {
        HeartbeatAck ack;
        ack.server_time =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
        (void)ch->send_message(ack);
    }
}

void MachinedApp::on_query(const Address& /*src*/, Channel* ch, const QueryMessage& msg)
{
    if (ch == nullptr)
        return;

    auto entries = process_registry_.find_by_type(msg.process_type);

    QueryResponse resp;
    resp.processes.reserve(entries.size());
    for (const auto& e : entries)
    {
        ProcessInfo info;
        info.process_type = e.process_type;
        info.name = e.name;
        info.internal_addr = e.internal_addr;
        info.external_addr = e.external_addr;
        info.pid = e.pid;
        info.load = e.load;
        resp.processes.push_back(std::move(info));
    }
    (void)ch->send_message(resp);
}

void MachinedApp::on_listener_register(const Address& /*src*/, Channel* ch,
                                       const ListenerRegister& msg)
{
    if (ch == nullptr)
        return;

    listener_manager_.add_listener(ch, msg.listener_type, msg.target_type);

    ListenerAck ack;
    ack.success = true;
    (void)ch->send_message(ack);
}

void MachinedApp::on_watcher_request(const Address& /*src*/, Channel* ch, const WatcherRequest& msg)
{
    watcher_forwarder_.handle_request(ch, msg);
}

void MachinedApp::on_watcher_reply(const Address& /*src*/, Channel* ch, const WatcherReply& msg)
{
    watcher_forwarder_.handle_reply(ch, msg);
}

// ============================================================================
// Connection lifecycle
// ============================================================================

void MachinedApp::on_accept(Channel& ch)
{
    ch.set_disconnect_callback([this](Channel& c) { on_disconnect(c); });
    ATLAS_LOG_DEBUG("MachinedApp: new connection from {}", ch.remote_address().to_string());
}

void MachinedApp::on_disconnect(Channel& ch)
{
    auto removed = process_registry_.unregister_by_channel(&ch);
    if (removed)
    {
        ATLAS_LOG_INFO("MachinedApp: process ({}, {}) disconnected",
                       static_cast<int>(removed->process_type), removed->name);

        DeathNotification notif;
        notif.process_type = removed->process_type;
        notif.name = removed->name;
        notif.internal_addr = removed->internal_addr;
        notif.reason = 1;  // connection_lost
        listener_manager_.notify_death(notif);
    }

    std::erase_if(heartbeat_entries_, [&ch](const HeartbeatEntry& e) { return e.channel == &ch; });
    listener_manager_.remove_all(&ch);
}

// ============================================================================
// Heartbeat timeout checking
// ============================================================================

void MachinedApp::check_heartbeat_timeouts()
{
    auto now = Clock::now();

    std::erase_if(heartbeat_entries_,
                  [&](const HeartbeatEntry& e)
                  {
                      if (now - e.last_heartbeat < kHeartbeatTimeout)
                          return false;

                      ATLAS_LOG_WARNING(
                          "MachinedApp: heartbeat timeout for channel {}",
                          e.channel != nullptr ? e.channel->remote_address().to_string() : "?");

                      auto removed = process_registry_.unregister_by_channel(e.channel);
                      if (removed)
                      {
                          DeathNotification notif;
                          notif.process_type = removed->process_type;
                          notif.name = removed->name;
                          notif.internal_addr = removed->internal_addr;
                          notif.reason = 2;  // timeout
                          listener_manager_.notify_death(notif);
                      }

                      listener_manager_.remove_all(e.channel);
                      return true;
                  });
}

}  // namespace atlas::machined
