#include "server/machined_client.hpp"

#include "foundation/log.hpp"
#include "foundation/time.hpp"
#include "network/channel.hpp"
#include "network/event_dispatcher.hpp"
#include "network/interface_table.hpp"
#include "network/network_interface.hpp"
#include "network/tcp_channel.hpp"
#include "network/udp_channel.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace atlas
{

MachinedClient::MachinedClient(EventDispatcher& dispatcher, NetworkInterface& network)
    : dispatcher_(dispatcher), network_(network)
{
}

MachinedClient::~MachinedClient() = default;

// ============================================================================
// Connection
// ============================================================================

auto MachinedClient::connect(const Address& machined_addr) -> bool
{
    if (!handlers_registered_)
    {
        register_handlers();
        handlers_registered_ = true;
    }

    auto result = network_.connect_tcp(machined_addr);
    if (!result)
    {
        ATLAS_LOG_ERROR("MachinedClient: connect failed: {}", result.error().message());
        return false;
    }
    channel_ = *result;
    ATLAS_LOG_INFO("MachinedClient: connecting to {}", machined_addr.to_string());
    return true;
}

auto MachinedClient::is_connected() const -> bool
{
    return channel_ != nullptr && channel_->is_connected();
}

// ============================================================================
// Registration
// ============================================================================

void MachinedClient::send_register(const ServerConfig& cfg)
{
    if (!is_connected())
    {
        ATLAS_LOG_WARNING("MachinedClient: send_register called but not connected");
        return;
    }

    machined::RegisterMessage msg;
    msg.protocol_version = machined::kProtocolVersion;
    msg.process_type = cfg.process_type;
    msg.name = cfg.process_name;
    msg.internal_port = cfg.internal_port;
    msg.external_port = cfg.external_port;
#if defined(_WIN32)
    msg.pid = static_cast<uint32_t>(_getpid());
#else
    msg.pid = static_cast<uint32_t>(getpid());
#endif

    if (auto r = channel_->send_message(msg); !r)
    {
        ATLAS_LOG_ERROR("MachinedClient: failed to send register: {}", r.error().message());
    }
}

void MachinedClient::send_deregister(const ServerConfig& cfg)
{
    if (!is_connected())
        return;

    machined::DeregisterMessage msg;
    msg.process_type = cfg.process_type;
    msg.name = cfg.process_name;
#if defined(_WIN32)
    msg.pid = static_cast<uint32_t>(_getpid());
#else
    msg.pid = static_cast<uint32_t>(getpid());
#endif

    if (auto r = channel_->send_message(msg); !r)
    {
        ATLAS_LOG_WARNING("MachinedClient: failed to send deregister: {}", r.error().message());
    }

    registered_ = false;
}

// ============================================================================
// Heartbeats
// ============================================================================

void MachinedClient::send_heartbeat(float load, uint32_t entity_count)
{
    if (!is_connected())
        return;

    machined::HeartbeatMessage msg;
    msg.load = load;
    msg.entity_count = entity_count;

    if (machined_heartbeat_udp_addr_.port() != 0)
    {
        // Fire-and-forget UDP heartbeat — no TCP round-trip, no ack expected
        auto udp_ch = network_.connect_udp(machined_heartbeat_udp_addr_);
        if (udp_ch)
        {
            (void)(*udp_ch)->send_message(msg);
            last_heartbeat_ = Clock::now();
            return;
        }
        // Fall through to TCP on failure
    }

    if (auto r = channel_->send_message(msg); !r)
    {
        ATLAS_LOG_WARNING("MachinedClient: failed to send heartbeat: {}", r.error().message());
    }
    last_heartbeat_ = Clock::now();
}

void MachinedClient::tick(float load, uint32_t entity_count)
{
    if (!is_connected())
        return;

    auto now = Clock::now();
    if (now - last_heartbeat_ >= kHeartbeatInterval)
    {
        send_heartbeat(load, entity_count);
    }
}

// ============================================================================
// Synchronous query
// ============================================================================

auto MachinedClient::query_sync(ProcessType type, Duration timeout)
    -> std::vector<machined::ProcessInfo>
{
    if (!is_connected())
        return {};

    machined::QueryMessage msg;
    msg.process_type = type;

    if (auto r = channel_->send_message(msg); !r)
    {
        ATLAS_LOG_ERROR("MachinedClient: query_sync failed to send: {}", r.error().message());
        return {};
    }

    sync_query_.emplace();
    auto deadline = Clock::now() + timeout;

    while (!sync_query_->done && Clock::now() < deadline)
    {
        dispatcher_.process_once();
    }

    auto result = std::move(sync_query_->result);
    sync_query_.reset();
    return result;
}

// ============================================================================
// Async query
// ============================================================================

void MachinedClient::query_async(ProcessType type, QueryCallback cb)
{
    if (!is_connected())
    {
        cb({});
        return;
    }

    async_query_cb_ = std::move(cb);

    machined::QueryMessage msg;
    msg.process_type = type;

    if (auto r = channel_->send_message(msg); !r)
    {
        ATLAS_LOG_ERROR("MachinedClient: query_async failed to send: {}", r.error().message());
        if (async_query_cb_)
        {
            async_query_cb_({});
            async_query_cb_ = nullptr;
        }
    }
}

// ============================================================================
// Listener subscriptions
// ============================================================================

void MachinedClient::subscribe(machined::ListenerType listener_type, ProcessType target_type,
                               BirthCallback on_birth, DeathCallback on_death)
{
    subscriptions_.push_back(
        {listener_type, target_type, std::move(on_birth), std::move(on_death)});

    if (is_connected())
    {
        machined::ListenerRegister msg;
        msg.listener_type = listener_type;
        msg.target_type = target_type;
        (void)channel_->send_message(msg);
    }
}

// ============================================================================
// Handler registration
// ============================================================================

void MachinedClient::register_handlers()
{
    auto& table = network_.interface_table();

    (void)table.register_typed_handler<machined::RegisterAck>(
        [this](const Address& src, Channel* ch, const machined::RegisterAck& msg)
        { on_register_ack(src, ch, msg); });

    (void)table.register_typed_handler<machined::HeartbeatAck>(
        [this](const Address& src, Channel* ch, const machined::HeartbeatAck& msg)
        { on_heartbeat_ack(src, ch, msg); });

    (void)table.register_typed_handler<machined::QueryResponse>(
        [this](const Address& src, Channel* ch, const machined::QueryResponse& msg)
        { on_query_response(src, ch, msg); });

    (void)table.register_typed_handler<machined::BirthNotification>(
        [this](const Address& src, Channel* ch, const machined::BirthNotification& msg)
        { on_birth_notification(src, ch, msg); });

    (void)table.register_typed_handler<machined::DeathNotification>(
        [this](const Address& src, Channel* ch, const machined::DeathNotification& msg)
        { on_death_notification(src, ch, msg); });

    (void)table.register_typed_handler<machined::ListenerAck>(
        [this](const Address& src, Channel* ch, const machined::ListenerAck& msg)
        { on_listener_ack(src, ch, msg); });

    (void)table.register_typed_handler<machined::WatcherResponse>(
        [this](const Address& src, Channel* ch, const machined::WatcherResponse& msg)
        { on_watcher_response(src, ch, msg); });
}

// ============================================================================
// Message handlers
// ============================================================================

void MachinedClient::on_register_ack(const Address& src, Channel* /*ch*/,
                                     const machined::RegisterAck& msg)
{
    if (msg.success)
    {
        registered_ = true;

        for (const auto& sub : subscriptions_)
        {
            machined::ListenerRegister listener_msg;
            listener_msg.listener_type = sub.listener_type;
            listener_msg.target_type = sub.target_type;
            if (channel_ != nullptr)
            {
                (void)channel_->send_message(listener_msg);
            }
        }

        if (msg.heartbeat_udp_port != 0)
        {
            machined_heartbeat_udp_addr_ = Address(src.ip(), msg.heartbeat_udp_port);
            ATLAS_LOG_INFO("MachinedClient: registered with machined — heartbeats via UDP {}:{}",
                           src.ip(), msg.heartbeat_udp_port);
        }
        else
        {
            ATLAS_LOG_INFO("MachinedClient: registered with machined (server_time={})",
                           msg.server_time);
        }
    }
    else
    {
        ATLAS_LOG_ERROR("MachinedClient: registration rejected: {}", msg.error_message);
    }
}

void MachinedClient::on_heartbeat_ack(const Address& /*src*/, Channel* /*ch*/,
                                      const machined::HeartbeatAck& /*msg*/)
{
    // No action needed; receipt confirms liveness
}

void MachinedClient::on_query_response(const Address& /*src*/, Channel* /*ch*/,
                                       const machined::QueryResponse& msg)
{
    if (sync_query_.has_value())
    {
        sync_query_->result = msg.processes;
        sync_query_->done = true;
        return;
    }
    if (async_query_cb_)
    {
        async_query_cb_(msg.processes);
        async_query_cb_ = nullptr;
    }
}

void MachinedClient::on_birth_notification(const Address& /*src*/, Channel* /*ch*/,
                                           const machined::BirthNotification& msg)
{
    for (const auto& sub : subscriptions_)
    {
        if (sub.target_type != msg.process_type)
            continue;
        if (sub.listener_type == machined::ListenerType::Death)
            continue;
        if (sub.on_birth)
            sub.on_birth(msg);
    }
}

void MachinedClient::on_death_notification(const Address& /*src*/, Channel* /*ch*/,
                                           const machined::DeathNotification& msg)
{
    for (const auto& sub : subscriptions_)
    {
        if (sub.target_type != msg.process_type)
            continue;
        if (sub.listener_type == machined::ListenerType::Birth)
            continue;
        if (sub.on_death)
            sub.on_death(msg);
    }
}

void MachinedClient::on_listener_ack(const Address& /*src*/, Channel* /*ch*/,
                                     const machined::ListenerAck& msg)
{
    if (!msg.success)
    {
        ATLAS_LOG_WARNING("MachinedClient: listener subscription was rejected");
    }
}

void MachinedClient::on_watcher_response(const Address& /*src*/, Channel* /*ch*/,
                                         const machined::WatcherResponse& msg)
{
    ATLAS_LOG_DEBUG("MachinedClient: watcher response rid={} found={} value='{}'", msg.request_id,
                    msg.found, msg.value);
}

}  // namespace atlas
