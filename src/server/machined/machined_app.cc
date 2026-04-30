#include "machined/machined_app.h"

#include <algorithm>

#include "foundation/clock.h"
#include "foundation/log.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "server/server_config.h"

namespace atlas::machined {

MachinedApp::MachinedApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network),
      watcher_forwarder_(process_registry_, [this](const Address& addr) {
        return this->Network().FindChannel(addr);
      }) {}

auto MachinedApp::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);
  MachinedApp app(dispatcher, network);
  return app.RunApp(argc, argv);
}

auto MachinedApp::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  const auto& cfg = Config();

  // Start TCP server on the machined internal port (for register/notify/query)
  Address listen_addr(0, cfg.internal_port);
  if (auto r = Network().StartTcpServer(listen_addr); !r) {
    ATLAS_LOG_CRITICAL("MachinedApp: failed to start TCP server: {}", r.Error().Message());
    return false;
  }

  // Install accept callback so we can attach disconnect handlers per-connection
  Network().SetAcceptCallback([this](Channel& ch) { OnAccept(ch); });

  // Start UDP socket for heartbeat datagrams on (internal_port + 1).
  // This avoids the TCP ack round-trip for high-frequency heartbeats.
  uint16_t udp_port = (cfg.internal_port > 0) ? static_cast<uint16_t>(cfg.internal_port + 1) : 0;
  if (udp_port > 0) {
    Address udp_addr(0, udp_port);
    if (auto r = Network().StartUdp(udp_addr); !r) {
      ATLAS_LOG_WARNING("MachinedApp: failed to start UDP heartbeat socket on port {}: {}",
                        udp_port, r.Error().Message());
      // Non-fatal: fall back to TCP-only heartbeats
      udp_port = 0;
    } else {
      heartbeat_udp_port_ = Network().UdpAddress().Port();
      ATLAS_LOG_INFO("MachinedApp: UDP heartbeat listening on port {}", heartbeat_udp_port_);
    }
  }

  // Register message handlers
  auto& table = Network().InterfaceTable();

  (void)table.RegisterTypedHandler<RegisterMessage>(
      [this](const Address& src, Channel* ch, const RegisterMessage& msg) {
        OnRegister(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<DeregisterMessage>(
      [this](const Address& src, Channel* ch, const DeregisterMessage& msg) {
        OnDeregister(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<HeartbeatMessage>(
      [this](const Address& src, Channel* ch, const HeartbeatMessage& msg) {
        OnHeartbeat(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<QueryMessage>(
      [this](const Address& src, Channel* ch, const QueryMessage& msg) { OnQuery(src, ch, msg); });

  (void)table.RegisterTypedHandler<ListenerRegister>(
      [this](const Address& src, Channel* ch, const ListenerRegister& msg) {
        OnListenerRegister(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<WatcherRequest>(
      [this](const Address& src, Channel* ch, const WatcherRequest& msg) {
        OnWatcherRequest(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<WatcherReply>(
      [this](const Address& src, Channel* ch, const WatcherReply& msg) {
        OnWatcherReply(src, ch, msg);
      });

  ATLAS_LOG_INFO("MachinedApp: TCP listening on {}", Network().TcpAddress().ToString());
  return true;
}

void MachinedApp::Fini() {
  // Notify all registered processes that machined is shutting down
  DeathNotification shutdown_notif;
  shutdown_notif.process_type = ProcessType::kMachined;
  shutdown_notif.name = "machined";
  shutdown_notif.reason = 0;

  process_registry_.ForEach([&](const ProcessEntry& entry) {
    if (entry.channel != nullptr && entry.channel->IsConnected()) {
      (void)entry.channel->SendMessage(shutdown_notif);
    }
  });

  ManagerApp::Fini();
}

void MachinedApp::RegisterWatchers() {
  ManagerApp::RegisterWatchers();

  auto& reg = GetWatcherRegistry();
  reg.Add<std::string>("machined/registered_processes", std::function<std::string()>{[this]() {
                         return std::to_string(process_registry_.Size());
                       }});
  reg.Add<std::string>("machined/listener_subscriptions", std::function<std::string()>{[this]() {
                         return std::to_string(listener_manager_.SubscriptionCount());
                       }});
  reg.Add<std::string>("machined/watcher_pending", std::function<std::string()>{[this]() {
                         return std::to_string(watcher_forwarder_.PendingCount());
                       }});
}

void MachinedApp::OnTickComplete() {
  CheckHeartbeatTimeouts();
  watcher_forwarder_.CheckTimeouts();
}

void MachinedApp::OnRegister(const Address& /*src*/, Channel* ch, const RegisterMessage& msg) {
  if (ch == nullptr) return;

  if (msg.protocol_version != kProtocolVersion) {
    RegisterAck ack;
    ack.success = false;
    ack.error_message = std::format("unsupported protocol version {}", msg.protocol_version);
    (void)ch->SendMessage(ack);
    return;
  }

  // Build ProcessEntry
  ProcessEntry entry;
  entry.process_type = msg.process_type;
  entry.name = msg.name;
  entry.internal_addr = Address(ch->RemoteAddress().Ip(), msg.internal_port);
  entry.external_addr = Address(ch->RemoteAddress().Ip(), msg.external_port);
  entry.pid = msg.pid;
  entry.channel = ch;

  RegisterAck ack;
  if (!process_registry_.RegisterProcess(entry)) {
    ack.success = false;
    ack.error_message =
        std::format("duplicate name ({}, {})", static_cast<int>(msg.process_type), msg.name);
    (void)ch->SendMessage(ack);
    return;
  }

  // Track heartbeat - TCP fallback if UDP socket not available
  heartbeat_entries_.push_back({ch, Clock::now()});

  ack.success = true;
  ack.server_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
  ack.heartbeat_udp_port = heartbeat_udp_port_;
  (void)ch->SendMessage(ack);

  // Notify listeners
  BirthNotification notif;
  notif.process_type = entry.process_type;
  notif.name = entry.name;
  notif.internal_addr = entry.internal_addr;
  notif.external_addr = entry.external_addr;
  notif.pid = entry.pid;
  listener_manager_.NotifyBirth(notif);

  ATLAS_LOG_INFO("MachinedApp: registered process ({}, {})", static_cast<int>(msg.process_type),
                 msg.name);
}

void MachinedApp::OnDeregister(const Address& /*src*/, Channel* ch, const DeregisterMessage& msg) {
  if (ch == nullptr) return;

  auto removed = process_registry_.UnregisterByName(msg.process_type, msg.name);
  if (!removed) {
    ATLAS_LOG_WARNING("MachinedApp: deregister for unknown ({}, {})",
                      static_cast<int>(msg.process_type), msg.name);
    return;
  }

  // Remove heartbeat entry
  std::erase_if(heartbeat_entries_, [ch](const HeartbeatEntry& e) { return e.channel == ch; });
  listener_manager_.RemoveAll(ch);

  DeathNotification notif;
  notif.process_type = removed->process_type;
  notif.name = removed->name;
  notif.internal_addr = removed->internal_addr;
  notif.reason = 0;  // normal deregister
  listener_manager_.NotifyDeath(notif);
}

void MachinedApp::OnHeartbeat(const Address& src, Channel* ch, const HeartbeatMessage& msg) {
  if (ch == nullptr) return;

  // UDP heartbeat: the source port is ephemeral, so correlate it back to the
  // TCP registration channel using pid+IP when available. IP-only matching
  // is kept as a backward-compatible fallback for older clients.
  Channel* tcp_ch = nullptr;
  if (msg.pid != 0) {
    tcp_ch = process_registry_.FindTcpChannelByPid(msg.pid, src.Ip());
  }
  if (tcp_ch == nullptr) {
    tcp_ch = process_registry_.FindTcpChannelByIp(src.Ip());
  }
  Channel* registry_ch = (tcp_ch != nullptr) ? tcp_ch : ch;

  process_registry_.UpdateLoad(registry_ch, msg.load, msg.entity_count);

  for (auto& e : heartbeat_entries_) {
    if (e.channel == registry_ch) {
      e.last_heartbeat = Clock::now();
      break;
    }
  }

  // Only send HeartbeatAck on TCP channels; UDP heartbeats are fire-and-forget.
  if (tcp_ch == nullptr) {
    HeartbeatAck ack;
    ack.server_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
    (void)ch->SendMessage(ack);
  }
}

void MachinedApp::OnQuery(const Address& /*src*/, Channel* ch, const QueryMessage& msg) {
  if (ch == nullptr) return;

  auto entries = process_registry_.FindByType(msg.process_type);

  QueryResponse resp;
  resp.processes.reserve(entries.size());
  for (const auto& e : entries) {
    ProcessInfo info;
    info.process_type = e.process_type;
    info.name = e.name;
    info.internal_addr = e.internal_addr;
    info.external_addr = e.external_addr;
    info.pid = e.pid;
    info.load = e.load;
    resp.processes.push_back(std::move(info));
  }
  (void)ch->SendMessage(resp);
}

void MachinedApp::OnListenerRegister(const Address& /*src*/, Channel* ch,
                                     const ListenerRegister& msg) {
  if (ch == nullptr) return;

  listener_manager_.AddListener(ch, msg.listener_type, msg.target_type);

  ListenerAck ack;
  ack.success = true;
  (void)ch->SendMessage(ack);

  if (msg.listener_type == ListenerType::kBirth || msg.listener_type == ListenerType::kBoth) {
    const auto kExisting = process_registry_.FindByType(msg.target_type);
    for (const auto& entry : kExisting) {
      BirthNotification notif;
      notif.process_type = entry.process_type;
      notif.name = entry.name;
      notif.internal_addr = entry.internal_addr;
      notif.external_addr = entry.external_addr;
      notif.pid = entry.pid;
      if (auto r = ch->SendMessage(notif); !r) {
        // Snapshot replay on Listener (re-)register; loss means listener
        // never learns about pre-existing peer until the next live
        // BirthNotification - could be a long wait.
        ATLAS_LOG_WARNING("Machined: BirthNotification snapshot send failed for {} (pid={}): {}",
                          entry.name, entry.pid, r.Error().Message());
      }
    }
  }
}

void MachinedApp::OnWatcherRequest(const Address& /*src*/, Channel* ch, const WatcherRequest& msg) {
  watcher_forwarder_.HandleRequest(ch, msg);
}

void MachinedApp::OnWatcherReply(const Address& /*src*/, Channel* ch, const WatcherReply& msg) {
  watcher_forwarder_.HandleReply(ch, msg);
}

void MachinedApp::OnAccept(Channel& ch) {
  ch.SetDisconnectCallback([this](Channel& c) { OnDisconnect(c); });
  ATLAS_LOG_DEBUG("MachinedApp: new connection from {}", ch.RemoteAddress().ToString());
}

void MachinedApp::OnDisconnect(Channel& ch) {
  auto removed = process_registry_.UnregisterByChannel(&ch);
  if (removed) {
    ATLAS_LOG_INFO("MachinedApp: process ({}, {}) disconnected",
                   static_cast<int>(removed->process_type), removed->name);

    DeathNotification notif;
    notif.process_type = removed->process_type;
    notif.name = removed->name;
    notif.internal_addr = removed->internal_addr;
    notif.reason = 1;  // connection_lost
    listener_manager_.NotifyDeath(notif);
  }

  std::erase_if(heartbeat_entries_, [&ch](const HeartbeatEntry& e) { return e.channel == &ch; });
  listener_manager_.RemoveAll(&ch);
}

void MachinedApp::CheckHeartbeatTimeouts() {
  auto now = Clock::now();

  std::erase_if(heartbeat_entries_, [&](const HeartbeatEntry& e) {
    if (now - e.last_heartbeat < kHeartbeatTimeout) return false;

    ATLAS_LOG_WARNING("MachinedApp: heartbeat timeout for channel {}",
                      e.channel != nullptr ? e.channel->RemoteAddress().ToString() : "?");

    auto removed = process_registry_.UnregisterByChannel(e.channel);
    if (removed) {
      DeathNotification notif;
      notif.process_type = removed->process_type;
      notif.name = removed->name;
      notif.internal_addr = removed->internal_addr;
      notif.reason = 2;  // timeout
      listener_manager_.NotifyDeath(notif);
    }

    listener_manager_.RemoveAll(e.channel);
    return true;
  });
}

}  // namespace atlas::machined
