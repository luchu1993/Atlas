#include "server/machined_client.h"

#include "foundation/clock.h"
#include "foundation/log.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "network/tcp_channel.h"
#include "network/udp_channel.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace atlas {

MachinedClient::MachinedClient(EventDispatcher& dispatcher, NetworkInterface& network)
    : dispatcher_(dispatcher), network_(network) {}

MachinedClient::~MachinedClient() = default;

// ============================================================================
// Connection
// ============================================================================

auto MachinedClient::Connect(const Address& machined_addr) -> bool {
  if (!handlers_registered_) {
    RegisterHandlers();
    handlers_registered_ = true;
  }

  auto result = network_.ConnectTcp(machined_addr);
  if (!result) {
    ATLAS_LOG_ERROR("MachinedClient: connect failed: {}", result.Error().Message());
    return false;
  }
  channel_ = *result;
  ATLAS_LOG_INFO("MachinedClient: connecting to {}", machined_addr.ToString());
  return true;
}

auto MachinedClient::IsConnected() const -> bool {
  return channel_ != nullptr && channel_->IsConnected();
}

// ============================================================================
// Registration
// ============================================================================

void MachinedClient::SendRegister(const ServerConfig& cfg) {
  if (!IsConnected()) {
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

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_ERROR("MachinedClient: failed to send register: {}", r.Error().Message());
  }
}

void MachinedClient::SendDeregister(const ServerConfig& cfg) {
  if (!IsConnected()) return;

  machined::DeregisterMessage msg;
  msg.process_type = cfg.process_type;
  msg.name = cfg.process_name;
#if defined(_WIN32)
  msg.pid = static_cast<uint32_t>(_getpid());
#else
  msg.pid = static_cast<uint32_t>(getpid());
#endif

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("MachinedClient: failed to send deregister: {}", r.Error().Message());
  }

  registered_ = false;
}

// ============================================================================
// Heartbeats
// ============================================================================

void MachinedClient::SendHeartbeat(float load, uint32_t entity_count) {
  if (!IsConnected()) return;

  machined::HeartbeatMessage msg;
  msg.load = load;
  msg.entity_count = entity_count;
#if defined(_WIN32)
  msg.pid = static_cast<uint32_t>(_getpid());
#else
  msg.pid = static_cast<uint32_t>(getpid());
#endif

  if (machined_heartbeat_udp_addr_.Port() != 0) {
    // Fire-and-forget UDP heartbeat — no TCP round-trip, no ack expected
    auto udp_ch = network_.ConnectUdp(machined_heartbeat_udp_addr_);
    if (udp_ch) {
      (void)(*udp_ch)->SendMessage(msg);
      last_heartbeat_ = Clock::now();
      return;
    }
    // Fall through to TCP on failure
  }

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("MachinedClient: failed to send heartbeat: {}", r.Error().Message());
  }
  last_heartbeat_ = Clock::now();
}

void MachinedClient::Tick(float load, uint32_t entity_count) {
  if (!IsConnected()) return;

  auto now = Clock::now();
  if (now - last_heartbeat_ >= kHeartbeatInterval) {
    SendHeartbeat(load, entity_count);
  }
}

// ============================================================================
// Synchronous query
// ============================================================================

auto MachinedClient::QuerySync(ProcessType type, Duration timeout)
    -> std::vector<machined::ProcessInfo> {
  if (!IsConnected()) return {};

  machined::QueryMessage msg;
  msg.process_type = type;

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_ERROR("MachinedClient: query_sync failed to send: {}", r.Error().Message());
    return {};
  }

  sync_query_.emplace();
  auto deadline = Clock::now() + timeout;

  while (!sync_query_->done && Clock::now() < deadline) {
    dispatcher_.ProcessOnce();
  }

  auto result = std::move(sync_query_->result);
  sync_query_.reset();
  return result;
}

// ============================================================================
// Async query
// ============================================================================

void MachinedClient::QueryAsync(ProcessType type, QueryCallback cb) {
  if (!IsConnected()) {
    cb({});
    return;
  }

  async_query_cb_ = std::move(cb);

  machined::QueryMessage msg;
  msg.process_type = type;

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_ERROR("MachinedClient: query_async failed to send: {}", r.Error().Message());
    if (async_query_cb_) {
      async_query_cb_({});
      async_query_cb_ = nullptr;
    }
  }
}

// ============================================================================
// Listener subscriptions
// ============================================================================

void MachinedClient::Subscribe(machined::ListenerType listener_type, ProcessType target_type,
                               BirthCallback on_birth, DeathCallback on_death) {
  subscriptions_.push_back({listener_type, target_type, std::move(on_birth), std::move(on_death)});

  if (IsConnected()) {
    machined::ListenerRegister msg;
    msg.listener_type = listener_type;
    msg.target_type = target_type;
    (void)channel_->SendMessage(msg);
  }
}

// ============================================================================
// Handler registration
// ============================================================================

void MachinedClient::RegisterHandlers() {
  auto& table = network_.InterfaceTable();

  (void)table.RegisterTypedHandler<machined::RegisterAck>(
      [this](const Address& src, Channel* ch, const machined::RegisterAck& msg) {
        OnRegisterAck(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<machined::HeartbeatAck>(
      [this](const Address& src, Channel* ch, const machined::HeartbeatAck& msg) {
        OnHeartbeatAck(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<machined::QueryResponse>(
      [this](const Address& src, Channel* ch, const machined::QueryResponse& msg) {
        OnQueryResponse(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<machined::BirthNotification>(
      [this](const Address& src, Channel* ch, const machined::BirthNotification& msg) {
        OnBirthNotification(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<machined::DeathNotification>(
      [this](const Address& src, Channel* ch, const machined::DeathNotification& msg) {
        OnDeathNotification(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<machined::ListenerAck>(
      [this](const Address& src, Channel* ch, const machined::ListenerAck& msg) {
        OnListenerAck(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<machined::WatcherResponse>(
      [this](const Address& src, Channel* ch, const machined::WatcherResponse& msg) {
        OnWatcherResponse(src, ch, msg);
      });
}

// ============================================================================
// Message handlers
// ============================================================================

void MachinedClient::OnRegisterAck(const Address& src, Channel* /*ch*/,
                                   const machined::RegisterAck& msg) {
  if (msg.success) {
    registered_ = true;

    for (const auto& sub : subscriptions_) {
      machined::ListenerRegister listener_msg;
      listener_msg.listener_type = sub.listener_type;
      listener_msg.target_type = sub.target_type;
      if (channel_ != nullptr) {
        (void)channel_->SendMessage(listener_msg);
      }
    }

    if (msg.heartbeat_udp_port != 0) {
      machined_heartbeat_udp_addr_ = Address(src.Ip(), msg.heartbeat_udp_port);
      ATLAS_LOG_INFO("MachinedClient: registered with machined — heartbeats via UDP {}:{}",
                     src.Ip(), msg.heartbeat_udp_port);
    } else {
      ATLAS_LOG_INFO("MachinedClient: registered with machined (server_time={})", msg.server_time);
    }
  } else {
    ATLAS_LOG_ERROR("MachinedClient: registration rejected: {}", msg.error_message);
  }
}

void MachinedClient::OnHeartbeatAck(const Address& /*src*/, Channel* /*ch*/,
                                    const machined::HeartbeatAck& /*msg*/) {
  // No action needed; receipt confirms liveness
}

void MachinedClient::OnQueryResponse(const Address& /*src*/, Channel* /*ch*/,
                                     const machined::QueryResponse& msg) {
  if (sync_query_.has_value()) {
    sync_query_->result = msg.processes;
    sync_query_->done = true;
    return;
  }
  if (async_query_cb_) {
    async_query_cb_(msg.processes);
    async_query_cb_ = nullptr;
  }
}

void MachinedClient::OnBirthNotification(const Address& /*src*/, Channel* /*ch*/,
                                         const machined::BirthNotification& msg) {
  for (const auto& sub : subscriptions_) {
    if (sub.target_type != msg.process_type) continue;
    if (sub.listener_type == machined::ListenerType::kDeath) continue;
    if (sub.on_birth) sub.on_birth(msg);
  }
}

void MachinedClient::OnDeathNotification(const Address& /*src*/, Channel* /*ch*/,
                                         const machined::DeathNotification& msg) {
  for (const auto& sub : subscriptions_) {
    if (sub.target_type != msg.process_type) continue;
    if (sub.listener_type == machined::ListenerType::kBirth) continue;
    if (sub.on_death) sub.on_death(msg);
  }
}

void MachinedClient::OnListenerAck(const Address& /*src*/, Channel* /*ch*/,
                                   const machined::ListenerAck& msg) {
  if (!msg.success) {
    ATLAS_LOG_WARNING("MachinedClient: listener subscription was rejected");
  }
}

void MachinedClient::OnWatcherResponse(const Address& /*src*/, Channel* /*ch*/,
                                       const machined::WatcherResponse& msg) {
  ATLAS_LOG_DEBUG("MachinedClient: watcher response rid={} found={} value='{}'", msg.request_id,
                  msg.found, msg.value);
}

}  // namespace atlas
