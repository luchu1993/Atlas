#include "machined/machined_app.h"

#include <algorithm>

#include "foundation/clock.h"
#include "foundation/log.h"
#include "network/broadcast.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "platform/process_launcher.h"
#include "server/common_messages.h"
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

  Address listen_addr(0, cfg.internal_port);
  if (auto r = Network().StartTcpServer(listen_addr); !r) {
    ATLAS_LOG_CRITICAL("MachinedApp: failed to start TCP server: {}", r.Error().Message());
    return false;
  }

  Network().SetAcceptCallback([this](Channel& ch) { OnAccept(ch); });

  // UDP heartbeats avoid the TCP ack round-trip for high-frequency updates.
  uint16_t udp_port = (cfg.internal_port > 0) ? static_cast<uint16_t>(cfg.internal_port + 1) : 0;
  if (udp_port > 0) {
    Address udp_addr(0, udp_port);
    if (auto r = Network().StartUdp(udp_addr); !r) {
      ATLAS_LOG_WARNING("MachinedApp: failed to start UDP heartbeat socket on port {}: {}",
                        udp_port, r.Error().Message());
      udp_port = 0;
    } else {
      heartbeat_udp_port_ = Network().UdpAddress().Port();
      ATLAS_LOG_INFO("MachinedApp: UDP heartbeat listening on port {}", heartbeat_udp_port_);
    }
  }

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

  (void)table.RegisterTypedHandler<ShutdownTarget>(
      [this](const Address& src, Channel* ch, const ShutdownTarget& msg) {
        OnShutdownTarget(src, ch, msg);
      });


  StartMesh();

  ATLAS_LOG_INFO("MachinedApp: TCP listening on {}", Network().TcpAddress().ToString());
  return true;
}

void MachinedApp::StartMesh() {
  const auto& cfg = Config();
  if (!cfg.mesh_enabled) return;
  if (cfg.internal_port == 0) {
    ATLAS_LOG_WARNING("MachinedApp: mesh enabled but internal_port is 0; mesh disabled");
    return;
  }

  const uint16_t mesh_port = static_cast<uint16_t>(cfg.internal_port + 2);
  const uint32_t self_ip = cfg.machined_address.Ip();
  const Address self_mesh =
      (self_ip != 0) ? Address(self_ip, mesh_port) : Address("127.0.0.1", mesh_port);
  const uint64_t incarnation =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());

  mesh_node_.emplace(Dispatcher(), self_mesh);
  mesh_node_->SetDeathCallback([this](const Address& dead) { OnMeshPeerDeath(dead); });
  mesh_node_->SetRegistryCallback(
      [this](const Address& owner, std::vector<ProcessInfo> processes) {
        mesh_registry_.UpdateOwner(owner, std::move(processes));
      });
  mesh_node_->SetPeerEventCallback([](const Address& peer, MachinedMesh::Observation obs) {
    ATLAS_LOG_INFO("MachinedApp: mesh peer {} {}", peer.ToString(),
                   obs == MachinedMesh::Observation::kRestarted ? "restarted" : "joined");
  });

  if (auto r = mesh_node_->Open(Address(0, mesh_port), LimitedBroadcastAddress(mesh_port),
                                incarnation);
      !r) {
    ATLAS_LOG_WARNING("MachinedApp: failed to open mesh on UDP port {}: {}", mesh_port,
                      r.Error().Message());
    mesh_node_.reset();
    return;
  }
  ATLAS_LOG_INFO("MachinedApp: mesh enabled on UDP port {} (incarnation {})", mesh_port,
                 incarnation);
}

auto MachinedApp::LocalProcessList() const -> std::vector<ProcessInfo> {
  std::vector<ProcessInfo> out;
  process_registry_.ForEach([&](const ProcessEntry& e) {
    ProcessInfo info;
    info.process_type = e.process_type;
    info.name = e.name;
    info.internal_addr = e.internal_addr;
    info.external_addr = e.external_addr;
    info.pid = e.pid;
    info.load = e.load;
    out.push_back(std::move(info));
  });
  return out;
}

void MachinedApp::BroadcastLocalRegistryIfDue(TimePoint now) {
  if (!mesh_node_) return;
  if (now - last_registry_broadcast_ < kMeshRegistryBroadcastInterval) return;
  mesh_node_->BroadcastRegistry(LocalProcessList());
  last_registry_broadcast_ = now;
}

void MachinedApp::OnMeshPeerDeath(const Address& dead) {
  ++mesh_dead_buddies_;
  auto gone = mesh_registry_.TakeOwner(dead);
  for (const auto& p : gone) {
    DeathNotification notif;
    notif.process_type = p.process_type;
    notif.name = p.name;
    notif.internal_addr = p.internal_addr;
    notif.reason = 2;  // the owning machined left the mesh, so its processes are gone
    listener_manager_.NotifyDeath(notif);
  }
  ATLAS_LOG_WARNING("MachinedApp: mesh peer {} died; evicted {} remote process(es)",
                    dead.ToString(), gone.size());
}

void MachinedApp::Fini() {
  mesh_node_.reset();

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
  reg.Add<std::string>("machined/mesh/enabled", std::function<std::string()>{[this]() {
                         return mesh_node_ ? "1" : "0";
                       }});
  reg.Add<std::string>("machined/mesh/incarnation", std::function<std::string()>{[this]() {
                         return mesh_node_ ? std::to_string(mesh_node_->Incarnation()) : "0";
                       }});
  reg.Add<std::string>("machined/mesh/peers", std::function<std::string()>{[this]() {
                         return mesh_node_ ? std::to_string(mesh_node_->PeerCount()) : "0";
                       }});
  reg.Add<std::string>("machined/mesh/dead_buddies", std::function<std::string()>{[this]() {
                         return std::to_string(mesh_dead_buddies_);
                       }});
  reg.Add<std::string>("machined/mesh/remote_processes", std::function<std::string()>{[this]() {
                         return std::to_string(mesh_registry_.ProcessCount());
                       }});
}

void MachinedApp::OnTickComplete() {
  CheckHeartbeatTimeouts();
  watcher_forwarder_.CheckTimeouts();
  if (mesh_node_) {
    const auto now = Clock::now();
    mesh_node_->Tick(now);
    BroadcastLocalRegistryIfDue(now);
  }
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

  if (auto existing = process_registry_.FindByName(msg.process_type, msg.name);
      existing && existing->pid != 0 && !IsProcessAlive(existing->pid)) {
    auto removed = process_registry_.UnregisterByName(msg.process_type, msg.name);
    if (removed) {
      std::erase_if(heartbeat_entries_, [&](const HeartbeatEntry& e) {
        return e.channel == removed->channel;
      });
      listener_manager_.RemoveAll(removed->channel);
      NotifyDeath(*removed, TakePendingShutdownReason(removed->channel, 2));
    }
  }

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

  heartbeat_entries_.push_back({ch, Clock::now()});

  ack.success = true;
  ack.server_time = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
  ack.heartbeat_udp_port = heartbeat_udp_port_;
  (void)ch->SendMessage(ack);

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

  Channel* removed_ch = removed->channel != nullptr ? removed->channel : ch;
  std::erase_if(heartbeat_entries_, [removed_ch](const HeartbeatEntry& e) {
    return e.channel == removed_ch;
  });
  listener_manager_.RemoveAll(removed_ch);

  NotifyDeath(*removed, TakePendingShutdownReason(removed_ch, 0));
}

void MachinedApp::OnHeartbeat(const Address& src, Channel* ch, const HeartbeatMessage& msg) {
  if (ch == nullptr) return;

  // UDP heartbeats use an ephemeral source port, so pid+IP links them
  // back to the TCP registration channel. IP-only is a legacy fallback.
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
  // Merge processes other hosts gossiped onto the mesh so a query resolves the
  // whole cluster, not just this machined's local registrations.
  for (auto& remote : mesh_registry_.FindByType(msg.process_type)) {
    resp.processes.push_back(std::move(remote));
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
        // Replay covers peers registered before listener startup; on send failure,
        // the listener waits for a future live birth event.
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

void MachinedApp::OnShutdownTarget(const Address& /*src*/, Channel* /*ch*/,
                                   const ShutdownTarget& msg) {
  msg::ShutdownRequest fwd;
  fwd.reason = msg.reason;

  auto deliver = [&](const ProcessEntry& entry) {
    if (entry.channel == nullptr || !entry.channel->IsConnected()) return;
    if (auto r = entry.channel->SendMessage(fwd); !r) {
      ATLAS_LOG_WARNING("MachinedApp: failed to forward shutdown to {}: {}", entry.name,
                        r.Error().Message());
    } else {
      pending_shutdown_reasons_[entry.channel] = msg.reason;
      ATLAS_LOG_INFO("MachinedApp: forwarded shutdown to ({}, {})",
                     static_cast<int>(entry.process_type), entry.name);
    }
  };

  if (!msg.target_name.empty()) {
    auto target = process_registry_.FindByName(msg.target_type, msg.target_name);
    if (!target) {
      ATLAS_LOG_WARNING("MachinedApp: shutdown target not found ({}, {})",
                        static_cast<int>(msg.target_type), msg.target_name);
      return;
    }
    deliver(*target);
    return;
  }

  for (const auto& entry : process_registry_.FindByType(msg.target_type)) {
    deliver(entry);
  }
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

    NotifyDeath(*removed, TakePendingShutdownReason(&ch, 1));
  } else {
    pending_shutdown_reasons_.erase(&ch);
  }

  std::erase_if(heartbeat_entries_, [&ch](const HeartbeatEntry& e) { return e.channel == &ch; });
  listener_manager_.RemoveAll(&ch);
}

void MachinedApp::CheckHeartbeatTimeouts() {
  auto now = Clock::now();

  // OnDisconnect re-enters via UnregisterByChannel / RemoveAll and mutates
  // heartbeat_entries_; collect first, side-effect outside the erase_if.
  std::vector<Channel*> expired;
  std::erase_if(heartbeat_entries_, [&](const HeartbeatEntry& e) {
    if (now - e.last_heartbeat < kHeartbeatTimeout) return false;
    expired.push_back(e.channel);
    return true;
  });

  for (Channel* ch : expired) {
    ATLAS_LOG_WARNING("MachinedApp: heartbeat timeout for channel {}",
                      ch != nullptr ? ch->RemoteAddress().ToString() : "?");

    auto removed = process_registry_.UnregisterByChannel(ch);
    if (removed) {
      NotifyDeath(*removed, TakePendingShutdownReason(ch, 2));
    } else {
      pending_shutdown_reasons_.erase(ch);
    }

    listener_manager_.RemoveAll(ch);
  }
}

void MachinedApp::NotifyDeath(const ProcessEntry& entry, uint8_t reason) {
  DeathNotification notif;
  notif.process_type = entry.process_type;
  notif.name = entry.name;
  notif.internal_addr = entry.internal_addr;
  notif.reason = reason;
  listener_manager_.NotifyDeath(notif);
}

auto MachinedApp::TakePendingShutdownReason(Channel* ch, uint8_t fallback) -> uint8_t {
  if (ch == nullptr) return fallback;
  auto it = pending_shutdown_reasons_.find(ch);
  if (it == pending_shutdown_reasons_.end()) return fallback;
  const uint8_t reason = it->second;
  pending_shutdown_reasons_.erase(it);
  return reason;
}

}  // namespace atlas::machined
