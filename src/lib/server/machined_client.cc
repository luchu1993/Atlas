#include "server/machined_client.h"

#include <algorithm>

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

namespace {
auto CurrentPid() -> uint32_t {
#if defined(_WIN32)
  return static_cast<uint32_t>(_getpid());
#else
  return static_cast<uint32_t>(getpid());
#endif
}
}  // namespace

MachinedClient::MachinedClient(EventDispatcher& dispatcher, NetworkInterface& network)
    : dispatcher_(dispatcher), network_(network) {}

MachinedClient::~MachinedClient() = default;

auto MachinedClient::Connect(const Address& machined_addr) -> bool {
  if (!handlers_registered_) {
    RegisterHandlers();
    handlers_registered_ = true;
  }

  machined_addr_ = machined_addr;
  reconnect_enabled_ = true;

  auto result = network_.ConnectTcp(machined_addr);
  if (!result) {
    ATLAS_LOG_ERROR("MachinedClient: connect failed: {}", result.Error().Message());
    next_reconnect_attempt_ = Clock::now() + reconnect_backoff_;
    return false;
  }
  channel_ = *result;
  ATLAS_LOG_INFO("MachinedClient: connecting to {}", machined_addr.ToString());
  return true;
}

auto MachinedClient::IsConnected() const -> bool {
  if (channel_ == nullptr) return false;
  // Re-validate against the registry: a condemned channel is removed from
  // channels_ and may already be freed, so the cached pointer can dangle
  // between Tick()s. Pointer compare without deref is safe even if stale.
  if (network_.FindChannel(machined_addr_) != channel_) return false;
  return channel_->IsConnected();
}

void MachinedClient::SendRegister(const ServerConfig& cfg) {
  cached_reg_ =
      RegistrationInfo{cfg.process_type, cfg.process_name, cfg.internal_port, cfg.external_port};

  if (!IsConnected()) {
    ATLAS_LOG_DEBUG("MachinedClient: send_register deferred until reconnect");
    return;
  }

  SendCachedRegister();
}

void MachinedClient::SendCachedRegister() {
  if (!cached_reg_ || !IsConnected()) return;

  machined::RegisterMessage msg;
  msg.protocol_version = machined::kProtocolVersion;
  msg.process_type = cached_reg_->process_type;
  msg.name = cached_reg_->name;
  msg.internal_port = cached_reg_->internal_port;
  msg.external_port = cached_reg_->external_port;
  msg.pid = CurrentPid();

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_ERROR("MachinedClient: failed to send register: {}", r.Error().Message());
  }
}

void MachinedClient::SendDeregister(const ServerConfig& cfg) {
  reconnect_enabled_ = false;
  cached_reg_.reset();

  if (!IsConnected()) return;

  machined::DeregisterMessage msg;
  msg.process_type = cfg.process_type;
  msg.name = cfg.process_name;
  msg.pid = CurrentPid();

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("MachinedClient: failed to send deregister: {}", r.Error().Message());
  }

  registered_ = false;
}

void MachinedClient::SendHeartbeat(float load, uint32_t entity_count) {
  if (!IsConnected()) return;

  machined::HeartbeatMessage msg;
  msg.load = load;
  msg.entity_count = entity_count;
  msg.pid = CurrentPid();

  if (machined_heartbeat_udp_addr_.Port() != 0) {
    auto udp_ch = network_.ConnectUdp(machined_heartbeat_udp_addr_);
    if (udp_ch) {
      (void)(*udp_ch)->SendMessage(msg);
      last_heartbeat_ = Clock::now();
      return;
    }
  }

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("MachinedClient: failed to send heartbeat: {}", r.Error().Message());
  }
  last_heartbeat_ = Clock::now();
}

void MachinedClient::Tick(float load, uint32_t entity_count) {
  DetectStaleChannel();

  if (!IsConnected()) {
    if (reconnect_enabled_ && cached_reg_ && machined_addr_.Port() != 0) {
      MaybeReconnect();
    }
    return;
  }

  auto now = Clock::now();
  if (now - last_heartbeat_ >= kHeartbeatInterval) {
    SendHeartbeat(load, entity_count);
  }
}

void MachinedClient::DetectStaleChannel() {
  if (channel_ == nullptr) return;
  // The channel was condemned/removed by NI when its peer disconnected;
  // FindChannel returns nullptr (or a fresh replacement) once that happens.
  if (network_.FindChannel(machined_addr_) != channel_) {
    HandleDisconnect();
  } else if (!channel_->IsConnected()) {
    HandleDisconnect();
  }
}

void MachinedClient::HandleDisconnect() {
  if (channel_ == nullptr && !registered_) return;
  ATLAS_LOG_WARNING("MachinedClient: connection to machined lost; reconnect={}",
                    reconnect_enabled_ ? "enabled" : "disabled");
  channel_ = nullptr;
  registered_ = false;
  machined_heartbeat_udp_addr_ = Address{};
  next_reconnect_attempt_ = Clock::now() + reconnect_backoff_;
}

void MachinedClient::MaybeReconnect() {
  auto now = Clock::now();
  if (now < next_reconnect_attempt_) return;

  ATLAS_LOG_INFO("MachinedClient: reconnect attempt to {}", machined_addr_.ToString());
  auto result = network_.ConnectTcp(machined_addr_);
  if (!result) {
    ATLAS_LOG_WARNING("MachinedClient: reconnect failed: {}", result.Error().Message());
    reconnect_backoff_ = std::min(reconnect_backoff_ * 2, kMaxReconnectBackoff);
    next_reconnect_attempt_ = now + reconnect_backoff_;
    return;
  }
  channel_ = *result;
  reconnect_backoff_ = kInitialReconnectBackoff;

  // Subscriptions replay inside OnRegisterAck once machined ACKs.
  SendCachedRegister();
}

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

auto MachinedClient::QueryWatcher(ProcessType target_type, std::string_view target_name,
                                  std::string_view watcher_path, WatcherCallback cb) -> uint32_t {
  if (!IsConnected()) {
    if (cb) cb(false, std::string(target_name), {});
    return 0;
  }

  uint32_t rid = next_watcher_request_id_++;
  if (cb) pending_watchers_.emplace(rid, std::move(cb));

  machined::WatcherRequest msg;
  msg.target_type = target_type;
  msg.target_name = std::string(target_name);
  msg.watcher_path = std::string(watcher_path);
  msg.request_id = rid;

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_ERROR("MachinedClient: failed to send watcher request: {}", r.Error().Message());
    auto it = pending_watchers_.find(rid);
    if (it != pending_watchers_.end()) {
      auto cb_local = std::move(it->second);
      pending_watchers_.erase(it);
      if (cb_local) cb_local(false, std::string(target_name), {});
    }
  }
  return rid;
}

void MachinedClient::RequestShutdownTarget(ProcessType target_type, std::string_view target_name,
                                           uint8_t reason) {
  if (!IsConnected()) {
    ATLAS_LOG_WARNING("MachinedClient: shutdown forward requested but not connected");
    return;
  }

  machined::ShutdownTarget msg;
  msg.target_type = target_type;
  msg.target_name = std::string(target_name);
  msg.reason = reason;

  if (auto r = channel_->SendMessage(msg); !r) {
    ATLAS_LOG_ERROR("MachinedClient: failed to send shutdown forward: {}", r.Error().Message());
  }
}

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
      ATLAS_LOG_INFO("MachinedClient: registered with machined - heartbeats via UDP {}:{}",
                     src.Ip(), msg.heartbeat_udp_port);
    } else {
      ATLAS_LOG_INFO("MachinedClient: registered with machined (server_time={})", msg.server_time);
    }
  } else {
    ATLAS_LOG_ERROR("MachinedClient: registration rejected: {}", msg.error_message);
  }
}

void MachinedClient::OnHeartbeatAck(const Address& /*src*/, Channel* /*ch*/,
                                    const machined::HeartbeatAck& /*msg*/) {}

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
  auto it = pending_watchers_.find(msg.request_id);
  if (it == pending_watchers_.end()) {
    ATLAS_LOG_DEBUG("MachinedClient: watcher response rid={} has no pending callback",
                    msg.request_id);
    return;
  }
  auto cb = std::move(it->second);
  pending_watchers_.erase(it);
  if (cb) cb(msg.found, msg.source_name, msg.value);
}

}  // namespace atlas
