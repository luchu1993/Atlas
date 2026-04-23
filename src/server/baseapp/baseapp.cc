#include "baseapp.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <span>
#include <vector>

#include "baseapp_messages.h"
#include "baseapp_native_provider.h"
#include "baseappmgr/baseappmgr_messages.h"
#include "cellapp/cellapp_messages.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "db/idatabase.h"
#include "dbapp/dbapp_messages.h"
#include "entitydef/entity_def_registry.h"
#include "foundation/log.h"
#include "loginapp/login_messages.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "network/reliable_udp.h"
#include "script/script_value.h"
#include "server/watcher.h"

namespace atlas {

namespace {

auto HasManagedEntityType(uint16_t type_id) -> bool {
  return EntityDefRegistry::Instance().FindById(type_id) != nullptr;
}

// DB-blob framing. Base's C# Serialize produces DATA_BASE-only bytes; the
// CELL_DATA subset arrives separately via BackupCellEntity. Persistence
// stitches the two together on write and unstitches on checkout so cell-
// scope persistent properties survive a save/load cycle.
//
// Layout (fixed-size prefix):
//     byte 0      0xA7   magic
//     byte 1      0x01   version
//     bytes 2..5  u32 LE base_len
//     bytes 6..   base_bytes
//     next        u32 LE cell_len
//     next        cell_bytes
//
// Legacy blobs lacking the magic fall back to "all base, no cell", which
// preserves old databases exactly as their bytes have always been read.
constexpr std::byte kDbBlobMagic{0xA7};
constexpr std::byte kDbBlobVersion{0x01};

auto EncodeDbBlob(std::span<const std::byte> base_bytes, std::span<const std::byte> cell_bytes)
    -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(2 + 4 + base_bytes.size() + 4 + cell_bytes.size());
  out.push_back(kDbBlobMagic);
  out.push_back(kDbBlobVersion);
  auto push_u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFF));
  };
  push_u32(static_cast<uint32_t>(base_bytes.size()));
  out.insert(out.end(), base_bytes.begin(), base_bytes.end());
  push_u32(static_cast<uint32_t>(cell_bytes.size()));
  out.insert(out.end(), cell_bytes.begin(), cell_bytes.end());
  return out;
}

// Decode a DB blob into (base_bytes, cell_bytes) spans referencing the
// input. Returns false only on structural corruption (truncated length
// prefix); legacy pre-L2 blobs without the magic return true with the
// full blob reported as base and an empty cell span.
auto DecodeDbBlob(std::span<const std::byte> blob, std::span<const std::byte>& base_out,
                  std::span<const std::byte>& cell_out) -> bool {
  if (blob.empty() || blob[0] != kDbBlobMagic) {
    // Legacy: whole blob is base, no cell part.
    base_out = blob;
    cell_out = {};
    return true;
  }
  if (blob.size() < 2 + 4) return false;  // magic + version + base_len
  // blob[1] is version. Only version 0x01 exists today; reject unknown
  // versions so a future layout bump can't be misread as v1.
  if (blob[1] != kDbBlobVersion) return false;
  auto read_u32 = [&](std::size_t off) -> uint32_t {
    return static_cast<uint32_t>(blob[off]) | (static_cast<uint32_t>(blob[off + 1]) << 8) |
           (static_cast<uint32_t>(blob[off + 2]) << 16) |
           (static_cast<uint32_t>(blob[off + 3]) << 24);
  };
  std::size_t off = 2;
  const uint32_t kBaseLen = read_u32(off);
  off += 4;
  if (off + kBaseLen + 4 > blob.size()) return false;
  base_out = blob.subspan(off, kBaseLen);
  off += kBaseLen;
  const uint32_t kCellLen = read_u32(off);
  off += 4;
  if (off + kCellLen > blob.size()) return false;
  cell_out = blob.subspan(off, kCellLen);
  return true;
}

}  // namespace

void BaseApp::LoadTracker::MarkTickStarted() {
  tick_started_ = Clock::now();
}

void BaseApp::LoadTracker::ObserveTickComplete(int update_hertz, const LoadSnapshot& snapshot) {
  if (tick_started_ == TimePoint{}) {
    return;
  }

  const int kSafeUpdateHertz = std::max(update_hertz, 1);
  const auto kWorkDuration = Clock::now() - tick_started_;
  const auto kExpectedTick = std::chrono::duration<double>(1.0 / kSafeUpdateHertz);
  const float kInstantaneous =
      std::clamp(static_cast<float>(kWorkDuration / kExpectedTick), 0.0f, 1.0f);
  const float kQueuePressureUnits =
      static_cast<float>(snapshot.pending_prepare_count + snapshot.deferred_login_count) +
      static_cast<float>(snapshot.pending_force_logoff_count + snapshot.logoff_in_flight_count) *
          0.5f +
      static_cast<float>(snapshot.detached_proxy_count) * 0.1f;
  const float kQueuePressure = std::min(0.35f, kQueuePressureUnits / 512.0f);
  const float kTarget = std::clamp(kInstantaneous + kQueuePressure, 0.0f, 1.0f);
  load_ += (kTarget - load_) * BaseApp::kLoadSmoothingBias;
}

auto BaseApp::LoadTracker::BuildReport(uint32_t app_id, const LoadSnapshot& snapshot) const
    -> baseappmgr::InformLoad {
  baseappmgr::InformLoad msg;
  msg.app_id = app_id;
  msg.load = load_;
  msg.entity_count = snapshot.entity_count;
  msg.proxy_count = snapshot.proxy_count;
  msg.pending_prepare_count = snapshot.pending_prepare_count;
  msg.pending_force_logoff_count = snapshot.pending_force_logoff_count;
  msg.detached_proxy_count = snapshot.detached_proxy_count;
  msg.logoff_in_flight_count = snapshot.logoff_in_flight_count;
  msg.deferred_login_count = snapshot.deferred_login_count;
  return msg;
}

// ============================================================================
// run — static entry point
// ============================================================================

auto BaseApp::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher;
  NetworkInterface internal_network(dispatcher);
  NetworkInterface external_network(dispatcher);
  BaseApp app(dispatcher, internal_network, external_network);
  return app.RunApp(argc, argv);
}

BaseApp::BaseApp(EventDispatcher& dispatcher, NetworkInterface& internal_network,
                 NetworkInterface& external_network)
    : EntityApp(dispatcher, internal_network),
      external_network_(external_network),
      cellapp_peers_(internal_network) {}

BaseApp::~BaseApp() = default;

// ============================================================================
// init
// ============================================================================

auto BaseApp::Init(int argc, char* argv[]) -> bool {
  if (!EntityApp::Init(argc, argv)) return false;

  const auto& cfg = Config();

  entity_mgr_.SetIdClient(&id_client_);

  // ---- Register internal message handlers --------------------------------
  RegisterInternalHandlers();

  // ---- Open external RUDP listener (client-facing) -----------------------
  if (cfg.external_port > 0) {
    Address ext_addr(0, cfg.external_port);
    auto listen_result =
        external_network_.StartRudpServer(ext_addr, NetworkInterface::InternetRudpProfile());
    if (!listen_result) {
      ATLAS_LOG_ERROR("BaseApp: failed to listen on external port {}: {}", cfg.external_port,
                      listen_result.Error().Message());
      return false;
    }
    ATLAS_LOG_INFO("BaseApp: external RUDP interface listening on port {}", cfg.external_port);
  }
  // P5: apply an inactivity timeout on accept so a client that goes dark
  // (crash / ungraceful close / network failure) is detected within
  // bounded time instead of lingering as a zombie Proxy forever. Mirrors
  // LoginApp's pattern; value is tuned for worldstate sessions that
  // normally send traffic continuously via move/echo streams.
  external_network_.SetAcceptCallback(
      [](Channel& ch) { ch.SetInactivityTimeout(std::chrono::seconds(10)); });
  external_network_.SetDisconnectCallback([this](Channel& ch) { OnExternalClientDisconnect(ch); });

  // ---- Subscribe to DBApp birth notification to connect ----------------
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kDbApp,
      [this](const machined::BirthNotification& n) {
        if (dbapp_channel_ == nullptr) {
          ATLAS_LOG_INFO("BaseApp: DBApp born at {}:{}, connecting via RUDP...",
                         n.internal_addr.Ip(), n.internal_addr.Port());
          auto ch = Network().ConnectRudpNocwnd(n.internal_addr);
          if (ch) dbapp_channel_ = static_cast<Channel*>(*ch);
        }
      },
      [this](const machined::DeathNotification& n) {
        (void)n;
        ATLAS_LOG_WARNING("BaseApp: DBApp died, clearing dbapp channel");
        dbapp_channel_ = nullptr;
        FailAllDbappPendingRequests("dbapp_disconnected");
      });

  // ---- Subscribe to CellApps -----------------------------------------
  // Delegated to CellAppPeerRegistry which handles Birth/Death + self-filter
  // consistently with CellApp's own peer list. BaseApp never self-registers
  // as a CellApp, so self_addr is a default Address (filter is a no-op).
  cellapp_peers_.Subscribe(GetMachinedClient(), /*self_addr=*/Address{});

  // ---- Subscribe to CellAppMgr ---------------------------------------
  // Connect on birth so scripts can call RequestCreateSpace. On death we
  // just clear the channel — pending create requests fail via the loop
  // below so callers don't hang.
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kCellAppMgr,
      [this](const machined::BirthNotification& n) {
        if (cellappmgr_channel_ != nullptr) return;
        auto ch = Network().ConnectRudpNocwnd(n.internal_addr);
        if (ch) {
          cellappmgr_channel_ = static_cast<Channel*>(*ch);
          ATLAS_LOG_INFO("BaseApp: CellAppMgr connected at {}:{}", n.internal_addr.Ip(),
                         n.internal_addr.Port());
        }
      },
      [this](const machined::DeathNotification& /*n*/) {
        ATLAS_LOG_WARNING("BaseApp: CellAppMgr died, clearing channel");
        cellappmgr_channel_ = nullptr;
        // Fail all pending space-create callbacks so callers don't hang.
        for (auto& [rid, cb] : pending_space_creates_) {
          if (cb) cb(/*success=*/false, /*space_id=*/0, Address{});
        }
        pending_space_creates_.clear();
      });

  // ---- Subscribe to BaseAppMgr and register ourselves ----------------
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBirth, ProcessType::kBaseAppMgr,
      [this](const machined::BirthNotification& n) {
        if (baseappmgr_channel_ == nullptr) {
          ATLAS_LOG_INFO("BaseApp: BaseAppMgr born at {}:{}, registering via RUDP...",
                         n.internal_addr.Ip(), n.internal_addr.Port());
          auto ch = Network().ConnectRudpNocwnd(n.internal_addr);
          if (!ch) {
            ATLAS_LOG_ERROR("BaseApp: failed to connect to BaseAppMgr");
            return;
          }
          baseappmgr_channel_ = static_cast<Channel*>(*ch);

          // Register ourselves — advertise the RUDP internal address so
          // LoginApp and other peers can connect_rudp() to reach us.
          baseappmgr::RegisterBaseApp reg;
          reg.internal_addr = Network().RudpAddress();
          reg.external_addr =
              Address(Network().RudpAddress().Ip(), external_network_.RudpAddress().Port());
          (void)baseappmgr_channel_->SendMessage(reg);
        }
      },
      nullptr);

  // ---- Register login-flow internal message handlers ------------------
  auto& table = Network().InterfaceTable();
  (void)table.RegisterTypedHandler<login::PrepareLogin>(
      [this](const Address& /*src*/, Channel* ch, const login::PrepareLogin& msg) {
        OnPrepareLogin(*ch, msg);
      });
  (void)table.RegisterTypedHandler<login::CancelPrepareLogin>(
      [this](const Address& /*src*/, Channel* ch, const login::CancelPrepareLogin& msg) {
        OnCancelPrepareLogin(*ch, msg);
      });
  (void)table.RegisterTypedHandler<baseapp::ClientEventSeqReport>(
      [this](const Address& /*src*/, Channel* /*ch*/, const baseapp::ClientEventSeqReport& msg) {
        OnClientEventSeqReport(msg);
      });
  (void)table.RegisterTypedHandler<baseapp::ForceLogoff>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::ForceLogoff& msg) {
        OnForceLogoff(*ch, msg);
      });
  (void)table.RegisterTypedHandler<baseapp::ForceLogoffAck>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::ForceLogoffAck& msg) {
        OnForceLogoffAck(*ch, msg);
      });
  (void)table.RegisterTypedHandler<baseappmgr::RegisterBaseAppAck>(
      [this](const Address& /*src*/, Channel* ch, const baseappmgr::RegisterBaseAppAck& msg) {
        OnRegisterBaseappAck(*ch, msg);
      });
  // ---- EntityID allocation from DBApp --------------------------------
  (void)table.RegisterTypedHandler<dbapp::GetEntityIdsAck>(
      [this](const Address& /*src*/, Channel* ch, const dbapp::GetEntityIdsAck& msg) {
        OnGetEntityIdsAck(*ch, msg);
      });

  // ---- Register external client handler -------------------------------
  auto& ext_table = external_network_.InterfaceTable();
  (void)ext_table.RegisterTypedHandler<baseapp::Authenticate>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::Authenticate& msg) {
        OnClientAuthenticate(*ch, msg);
      });
  (void)ext_table.RegisterTypedHandler<baseapp::ClientBaseRpc>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::ClientBaseRpc& msg) {
        OnClientBaseRpc(*ch, msg);
      });
  (void)ext_table.RegisterTypedHandler<baseapp::ClientCellRpc>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::ClientCellRpc& msg) {
        OnClientCellRpc(*ch, msg);
      });

  ATLAS_LOG_INFO("BaseApp: initialised");
  return true;
}

// ============================================================================
// fini
// ============================================================================

void BaseApp::Fini() {
  entity_mgr_.ForEach([this](const BaseEntity& ent) {
    if (ent.Dbid() != kInvalidDBID) ReleaseCheckout(ent.Dbid(), ent.TypeId());
  });
  entity_mgr_.FlushDestroyed();
  EntityApp::Fini();
}

// ============================================================================
// OnTickComplete
// ============================================================================

void BaseApp::OnEndOfTick() {
  load_tracker_.MarkTickStarted();
}

void BaseApp::OnTickComplete() {
  entity_mgr_.FlushDestroyed();
  entity_mgr_.CleanupRetiredSessions();
  ExpireDetachedProxies();
  FlushClientDeltas();
  EmitBaselineSnapshots();
  UpdateLoadEstimate();
  ReportLoadToBaseAppMgr();
  CleanupExpiredPendingRequests();
  MaybeRequestMoreIds();

  EntityApp::OnTickComplete();
}

// ============================================================================
// create_native_provider
// ============================================================================

auto BaseApp::CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> {
  auto provider = std::make_unique<BaseAppNativeProvider>(*this);
  native_provider_ = provider.get();
  return provider;
}

// ============================================================================
// register_watchers
// ============================================================================

void BaseApp::RegisterWatchers() {
  EntityApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<float>("baseapp/load",
                std::function<float()>([this] { return load_tracker_.CurrentLoad(); }));
  wr.Add<std::size_t>("baseapp/entity_count",
                      std::function<std::size_t()>([this] { return entity_mgr_.Size(); }));
  wr.Add<std::size_t>("baseapp/proxy_count",
                      std::function<std::size_t()>([this] { return entity_mgr_.ProxyCount(); }));
  wr.Add<std::size_t>("baseapp/client_binding_count",
                      std::function<std::size_t()>([this] { return entity_client_index_.size(); }));
  wr.Add<uint64_t>("baseapp/auth_success_total",
                   std::function<uint64_t()>([this] { return auth_success_total_; }));
  wr.Add<uint64_t>("baseapp/auth_fail_total",
                   std::function<uint64_t()>([this] { return auth_fail_total_; }));
  wr.Add<uint64_t>("baseapp/force_logoff_total",
                   std::function<uint64_t()>([this] { return force_logoff_total_; }));
  wr.Add<uint64_t>("baseapp/fast_relogin_total",
                   std::function<uint64_t()>([this] { return fast_relogin_total_; }));
  wr.Add<uint64_t>("baseapp/detached_relogin_total",
                   std::function<uint64_t()>([this] { return detached_relogin_total_; }));
  wr.Add<std::size_t>("baseapp/pending_prepare_count",
                      std::function<std::size_t()>([this] { return pending_logins_.size(); }));
  wr.Add<std::size_t>("baseapp/pending_force_logoff_count", std::function<std::size_t()>([this] {
                        return pending_force_logoffs_.size();
                      }));
  wr.Add<std::size_t>("baseapp/deferred_login_checkout_count", std::function<std::size_t()>([this] {
                        return DeferredLoginCheckoutCount();
                      }));
  wr.Add<std::size_t>("baseapp/detached_proxy_count",
                      std::function<std::size_t()>([this] { return detached_proxies_.size(); }));
  wr.Add<uint64_t>("baseapp/client_event_seq_gaps_total",
                   std::function<uint64_t()>([this] { return client_event_seq_gaps_total_; }));
  wr.Add<std::size_t>("baseapp/logoff_in_flight_count", std::function<std::size_t()>([this] {
                        return logoff_entities_in_flight_.size();
                      }));
  wr.Add<std::size_t>("baseapp/canceled_checkout_count", std::function<std::size_t()>([this] {
                        return canceled_login_checkouts_.size();
                      }));
  wr.Add<uint64_t>("baseapp/canceled_checkout_total",
                   std::function<uint64_t()>([this] { return canceled_checkout_total_; }));
  wr.Add<uint64_t>("baseapp/prepared_login_timeout_total",
                   std::function<uint64_t()>([this] { return prepared_login_timeout_total_; }));
  wr.Add<bool>("baseapp/dbapp_connected",
               std::function<bool()>([this] { return dbapp_channel_ != nullptr; }));
  wr.Add<uint64_t>("baseapp/delta_bytes_sent_total",
                   std::function<uint64_t()>([this] { return delta_bytes_sent_total_; }));
  wr.Add<uint64_t>("baseapp/delta_bytes_deferred_total", std::function<uint64_t()>([this] {
                     uint64_t total = delta_bytes_deferred_total_;
                     for (auto& [_, fwd] : client_delta_forwarders_)
                       total += fwd.GetStats().bytes_deferred;
                     return total;
                   }));
  wr.Add<std::size_t>("baseapp/delta_queue_depth", std::function<std::size_t()>([this] {
                        std::size_t depth = 0;
                        for (auto& [_, fwd] : client_delta_forwarders_) depth += fwd.QueueDepth();
                        return depth;
                      }));
  wr.Add<uint64_t>("baseapp/reliable_delta_bytes_sent_total",
                   std::function<uint64_t()>([this] { return reliable_delta_bytes_sent_total_; }));
  wr.Add<uint64_t>("baseapp/reliable_delta_messages_sent_total", std::function<uint64_t()>([this] {
                     return reliable_delta_messages_sent_total_;
                   }));
  wr.Add<uint64_t>("baseapp/baseline_messages_sent_total",
                   std::function<uint64_t()>([this] { return baseline_messages_sent_total_; }));
  wr.Add<uint64_t>("baseapp/baseline_bytes_sent_total",
                   std::function<uint64_t()>([this] { return baseline_bytes_sent_total_; }));
}

// ============================================================================
// register_internal_handlers
// ============================================================================

void BaseApp::RegisterInternalHandlers() {
  auto& table = Network().InterfaceTable();

  (void)table.RegisterTypedHandler<baseapp::CreateBase>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::CreateBase& msg) {
        OnCreateBase(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::CreateBaseFromDB>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::CreateBaseFromDB& msg) {
        OnCreateBaseFromDb(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::AcceptClient>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::AcceptClient& msg) {
        OnAcceptClient(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::CellEntityCreated>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::CellEntityCreated& msg) {
        OnCellEntityCreated(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::CellEntityDestroyed>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::CellEntityDestroyed& msg) {
        OnCellEntityDestroyed(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::CurrentCell>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::CurrentCell& msg) {
        OnCurrentCell(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::CellAppDeath>(
      [this](const Address& /*src*/, Channel* /*ch*/, const baseapp::CellAppDeath& msg) {
        OnCellAppDeath(msg);
      });

  (void)table.RegisterTypedHandler<baseapp::CellRpcForward>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::CellRpcForward& msg) {
        OnCellRpcForward(*ch, msg);
      });

  // CellAppMgr replies to CreateSpaceRequest.
  (void)table.RegisterTypedHandler<cellappmgr::SpaceCreatedResult>(
      [this](const Address& /*src*/, Channel* ch, const cellappmgr::SpaceCreatedResult& msg) {
        OnSpaceCreatedResult(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::SelfRpcFromCell>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::SelfRpcFromCell& msg) {
        OnSelfRpcFromCell(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::BroadcastRpcFromCell>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::BroadcastRpcFromCell& msg) {
        OnBroadcastRpcFromCell(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::ReplicatedDeltaFromCell>(
      [this](const Address& /*src*/, Channel* ch, const baseapp::ReplicatedDeltaFromCell& msg) {
        OnReplicatedDeltaFromCell(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::ReplicatedReliableDeltaFromCell>(
      [this](const Address& /*src*/, Channel* ch,
             const baseapp::ReplicatedReliableDeltaFromCell& msg) {
        OnReplicatedReliableDeltaFromCell(*ch, msg);
      });

  (void)table.RegisterTypedHandler<baseapp::BackupCellEntity>(
      [this](const Address& /*src*/, Channel* /*ch*/, const baseapp::BackupCellEntity& msg) {
        OnBackupCellEntity(msg);
      });

  (void)table.RegisterTypedHandler<baseapp::ReplicatedBaselineFromCell>(
      [this](const Address& /*src*/, Channel* /*ch*/,
             const baseapp::ReplicatedBaselineFromCell& msg) {
        OnReplicatedBaselineFromCell(msg);
      });

  // ---- WriteEntityAck (DBApp → BaseApp) ----------------------------------
  (void)table.RegisterTypedHandler<dbapp::WriteEntityAck>(
      [this](const Address& /*src*/, Channel* /*ch*/, const dbapp::WriteEntityAck& msg) {
        auto logoff_it = pending_logoff_writes_.find(msg.request_id);
        if (logoff_it != pending_logoff_writes_.end()) {
          const PendingLogoffWrite kPendingWrite = logoff_it->second;
          pending_logoff_writes_.erase(logoff_it);
          logoff_entities_in_flight_.erase(kPendingWrite.entity_id);

          if (!msg.success) {
            ATLAS_LOG_ERROR(
                "BaseApp: logoff persist failed request_id={} entity_id={} "
                "dbid={} error={}",
                msg.request_id, kPendingWrite.entity_id, kPendingWrite.dbid, msg.error);

            if (kPendingWrite.continuation_request_id != 0) {
              FailPendingForceLogoff(kPendingWrite.continuation_request_id,
                                     "force_logoff_persist_failed");
            }
            if (auto waiter_it = pending_local_force_logoff_waiters_.find(kPendingWrite.entity_id);
                waiter_it != pending_local_force_logoff_waiters_.end()) {
              for (uint32_t waiter_request_id : waiter_it->second) {
                FailPendingForceLogoff(waiter_request_id, "force_logoff_persist_failed");
              }
              pending_local_force_logoff_waiters_.erase(waiter_it);
            }
            FailDeferredPrepareLogins(kPendingWrite.entity_id, "force_logoff_persist_failed");
            FlushRemoteForceLogoffAcks(kPendingWrite.entity_id, false);
            FinishLoginFlow(kPendingWrite.dbid);
            return;
          }

          if (!FinalizeForceLogoff(kPendingWrite.entity_id)) {
            if (kPendingWrite.continuation_request_id != 0) {
              FailPendingForceLogoff(kPendingWrite.continuation_request_id,
                                     "force_logoff_destroy_failed");
            }
            if (auto waiter_it = pending_local_force_logoff_waiters_.find(kPendingWrite.entity_id);
                waiter_it != pending_local_force_logoff_waiters_.end()) {
              for (uint32_t waiter_request_id : waiter_it->second) {
                FailPendingForceLogoff(waiter_request_id, "force_logoff_destroy_failed");
              }
              pending_local_force_logoff_waiters_.erase(waiter_it);
            }
            FailDeferredPrepareLogins(kPendingWrite.entity_id, "force_logoff_destroy_failed");
            FlushRemoteForceLogoffAcks(kPendingWrite.entity_id, false);
            FinishLoginFlow(kPendingWrite.dbid);
            return;
          }

          const bool kResumedDeferred = ResumeDeferredPrepareLogins(kPendingWrite.entity_id);

          if (auto waiter_it = pending_local_force_logoff_waiters_.find(kPendingWrite.entity_id);
              waiter_it != pending_local_force_logoff_waiters_.end()) {
            for (uint32_t waiter_request_id : waiter_it->second) {
              if (pending_force_logoffs_.contains(waiter_request_id)) {
                ContinueLoginAfterForceLogoff(waiter_request_id);
              }
            }
            pending_local_force_logoff_waiters_.erase(waiter_it);
          }

          FlushRemoteForceLogoffAcks(kPendingWrite.entity_id, true);

          if (kPendingWrite.continuation_request_id != 0 &&
              pending_force_logoffs_.contains(kPendingWrite.continuation_request_id)) {
            ContinueLoginAfterForceLogoff(kPendingWrite.continuation_request_id);
          } else if (!kResumedDeferred) {
            FinishLoginFlow(kPendingWrite.dbid);
          }
          return;
        }

        auto* ent = entity_mgr_.Find(msg.request_id);
        if (ent) {
          if (msg.success && msg.dbid != kInvalidDBID) {
            (void)entity_mgr_.AssignDbid(ent->EntityId(), msg.dbid);
          }
          ent->OnWriteAck(msg.dbid, msg.success);
        }
      });

  // ---- CheckoutEntityAck (DBApp → BaseApp) --------------------------------
  (void)table.RegisterTypedHandler<dbapp::CheckoutEntityAck>(
      [this](const Address& /*src*/, Channel* /*ch*/, const dbapp::CheckoutEntityAck& msg) {
        if (auto canceled_it = canceled_login_checkouts_.find(msg.request_id);
            canceled_it != canceled_login_checkouts_.end()) {
          if (msg.status == dbapp::CheckoutStatus::kSuccess && msg.dbid != kInvalidDBID) {
            ReleaseCheckout(msg.dbid, canceled_it->second.type_id);
          }
          canceled_login_checkouts_.erase(canceled_it);
          return;
        }

        // Check if this is a login-flow checkout (request_id is a pending key)
        auto login_it = pending_logins_.find(msg.request_id);
        if (login_it != pending_logins_.end()) {
          PendingLogin pending = login_it->second;
          pending_logins_.erase(login_it);

          if (msg.status != dbapp::CheckoutStatus::kSuccess) {
            if (msg.status == dbapp::CheckoutStatus::kAlreadyCheckedOut &&
                RetryLoginAfterCheckoutConflict(std::move(pending), msg.dbid, msg.holder_addr)) {
              return;
            }

            login::PrepareLoginResult reply;
            reply.request_id = pending.login_request_id;
            ATLAS_LOG_ERROR("BaseApp: login checkout failed (request_id={} status={} holder={}:{})",
                            msg.request_id, static_cast<int>(msg.status), msg.holder_addr.Ip(),
                            msg.holder_addr.Port());
            reply.success = false;
            reply.error = (msg.status == dbapp::CheckoutStatus::kAlreadyCheckedOut)
                              ? "already_checked_out"
                              : (msg.error.empty() ? "checkout_failed" : msg.error);
            if (!pending.reply_sent) {
              SendPrepareLoginResult(pending.loginapp_addr, reply);
            }
            FinishLoginFlow(pending.dbid);
            return;
          }

          CompletePrepareLoginFromCheckout(
              std::move(pending), msg.dbid, pending.type_id,
              std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg.blob.data()),
                                         msg.blob.size()));
          return;
        }

        // Fallback: CreateBaseFromDB checkout — request_id == entity_id
        const EntityID kEid = msg.request_id;
        if (msg.status != dbapp::CheckoutStatus::kSuccess) {
          ATLAS_LOG_ERROR("BaseApp: checkout failed (entity_id={} status={})", kEid,
                          static_cast<int>(msg.status));
          entity_mgr_.Destroy(kEid);
          return;
        }
        auto* ent = entity_mgr_.Find(kEid);
        if (!ent) return;
        // L2: split the DB blob into its base- and cell-scope halves.
        // Base bytes feed the C# Base.Deserialize path via RestoreEntity;
        // cell bytes get stashed on the Proxy so the next CreateCellEntity
        // can hydrate the cell side through script_init_data.
        std::span<const std::byte> base_span, cell_span;
        if (!DecodeDbBlob(std::span<const std::byte>(msg.blob.data(), msg.blob.size()), base_span,
                          cell_span)) {
          ATLAS_LOG_ERROR("BaseApp: checkout blob corrupt (entity_id={} dbid={})", kEid, msg.dbid);
          entity_mgr_.Destroy(kEid);
          ReleaseCheckout(msg.dbid, ent->TypeId());
          return;
        }
        ent->SetEntityData(std::vector<std::byte>(base_span.begin(), base_span.end()));
        ent->SetCellBackupData(std::vector<std::byte>(cell_span.begin(), cell_span.end()));
        if (!RestoreManagedEntity(kEid, ent->TypeId(), msg.dbid, base_span)) {
          (void)NotifyManagedEntityDestroyed(kEid, "CreateBaseFromDB rollback");
          entity_mgr_.Destroy(kEid);
          ReleaseCheckout(msg.dbid, ent->TypeId());
        }
      });

  (void)table.RegisterTypedHandler<dbapp::AbortCheckoutAck>(
      [this](const Address& /*src*/, Channel* /*ch*/, const dbapp::AbortCheckoutAck& msg) {
        canceled_login_checkouts_.erase(msg.request_id);
      });
}

// ============================================================================
// Message handlers
// ============================================================================

void BaseApp::OnCreateBase(Channel& /*ch*/, const baseapp::CreateBase& msg) {
  const auto& defs = EntityDefs();
  auto* type = defs.FindById(msg.type_id);
  if (!type) {
    ATLAS_LOG_ERROR("BaseApp: CreateBase: unknown type_id {}", msg.type_id);
    return;
  }
  bool has_client = type->has_client;
  auto* ent = entity_mgr_.Create(msg.type_id, has_client);
  if (!ent) {
    ATLAS_LOG_ERROR("BaseApp: CreateBase: EntityID range exhausted for type_id {}", msg.type_id);
    return;
  }

  ATLAS_LOG_DEBUG("BaseApp: created entity id={} type={}", ent->EntityId(), msg.type_id);
}

void BaseApp::OnCreateBaseFromDb(Channel& /*ch*/, const baseapp::CreateBaseFromDB& msg) {
  if (!dbapp_channel_) {
    ATLAS_LOG_ERROR("BaseApp: CreateBaseFromDB: no DBApp connection");
    return;
  }
  const auto& defs = EntityDefs();
  auto* type = defs.FindById(msg.type_id);
  bool has_client = type ? type->has_client : false;
  auto* ent = entity_mgr_.Create(msg.type_id, has_client, msg.dbid);
  if (!ent) {
    ATLAS_LOG_ERROR("BaseApp: CreateBaseFromDB: EntityID range exhausted for type_id {} dbid {}",
                    msg.type_id, msg.dbid);
    return;
  }

  dbapp::CheckoutEntity req;
  req.mode = msg.identifier.empty() ? dbapp::LoadMode::kByDbid : dbapp::LoadMode::kByName;
  req.type_id = msg.type_id;
  req.dbid = msg.dbid;
  req.identifier = msg.identifier;
  req.entity_id = ent->EntityId();
  (void)dbapp_channel_->SendMessage(req);
}

void BaseApp::OnAcceptClient(Channel& /*ch*/, const baseapp::AcceptClient& msg) {
  auto* proxy = entity_mgr_.FindProxy(msg.dest_entity_id);
  if (!proxy) {
    ATLAS_LOG_WARNING("BaseApp: AcceptClient: entity {} not a Proxy", msg.dest_entity_id);
    return;
  }
  if (!entity_mgr_.AssignSessionKey(proxy->EntityId(), msg.session_key)) {
    ATLAS_LOG_ERROR("BaseApp: AcceptClient: failed to bind session key to entity {}",
                    msg.dest_entity_id);
    return;
  }
  ClearDetachedGrace(proxy->EntityId());
  ATLAS_LOG_DEBUG("BaseApp: entity {} ready for client", msg.dest_entity_id);
}

void BaseApp::OnCellEntityCreated(Channel& ch, const baseapp::CellEntityCreated& msg) {
  auto* ent = entity_mgr_.Find(msg.base_entity_id);
  if (!ent) return;
  // CellApp fills msg.cell_addr from its own Network().RudpAddress(), which
  // for INADDR_ANY binds reports 0.0.0.0:<port> — useless for routing. The
  // channel's RemoteAddress is the actual peer we just received from, and
  // matches what machined's Birth notification put in cellapp_peers_, so
  // use it when the wire value is unresolved.
  Address cell_addr = msg.cell_addr;
  if (cell_addr.Ip() == 0) {
    cell_addr = ch.RemoteAddress();
  }
  ent->SetCell(msg.cell_entity_id, cell_addr);
  ATLAS_LOG_DEBUG("BaseApp: entity {} has cell at {}:{}", msg.base_entity_id, cell_addr.Ip(),
                  cell_addr.Port());

  // Tell the client its cell is ready. Without this the client has no way
  // to know when its ClientCellRpc will actually route vs. be dropped by
  // the "no cell channel for target entity" path. A Proxy that doesn't
  // have a client yet (e.g. entity created via CreateBaseEntity before
  // GiveClientTo) gets nothing here — if a client binds later, CellReady
  // doesn't fire retroactively, but in the world_stress script flow
  // GiveClientTo runs strictly before the CellApp ack can arrive.
  auto* proxy = entity_mgr_.FindProxy(msg.base_entity_id);
  if (proxy && proxy->HasClient()) {
    // PR 34 C3 race fix: client may have been bound BEFORE this cell
    // ack arrived (login's GiveClientTo runs before CellApp replies),
    // in which case BindClient couldn't route EnableWitness — no
    // cell_addr yet. Now that SetCell above populated it, send the
    // EnableWitness we skipped.
    if (auto* cell_ch = ResolveCellChannelForEntity(msg.base_entity_id)) {
      cellapp::EnableWitness ew;
      ew.base_entity_id = msg.base_entity_id;
      (void)cell_ch->SendMessage(ew);
    }
    if (auto* client_ch = ResolveClientChannel(msg.base_entity_id)) {
      baseapp::CellReady ready;
      ready.entity_id = msg.base_entity_id;
      (void)client_ch->SendMessage(ready);
    }
  }
}

void BaseApp::OnCellEntityDestroyed(Channel& /*ch*/, const baseapp::CellEntityDestroyed& msg) {
  auto* ent = entity_mgr_.Find(msg.base_entity_id);
  if (!ent) return;
  ent->ClearCell();
}

void BaseApp::OnCurrentCell(Channel& /*ch*/, const baseapp::CurrentCell& msg) {
  auto* ent = entity_mgr_.Find(msg.base_entity_id);
  if (!ent) return;
  ent->SetCell(msg.cell_entity_id, msg.cell_addr, msg.epoch);
}

// For every BaseEntity that was Real-hosted on the dead CellApp, issue a
// fresh CreateCellEntity to the rehome target carrying the last-cached
// cell_backup_data as script_init_data — the new CellApp's RestoreEntity
// callback rehydrates the C# instance from that blob. Witness re-attach
// happens when the new host's CellEntityCreated ack lands; the client
// stays connected throughout (AoI pauses until the Real is back up).
void BaseApp::OnCellAppDeath(const baseapp::CellAppDeath& msg) {
  ATLAS_LOG_WARNING("BaseApp: CellAppDeath dead_addr={}:{} rehome_spaces={}", msg.dead_addr.Ip(),
                    msg.dead_addr.Port(), msg.rehomes.size());

  // Flatten the rehome list into a map for O(1) per-entity lookup.
  std::unordered_map<SpaceID, Address> space_to_new_host;
  space_to_new_host.reserve(msg.rehomes.size());
  for (const auto& [sid, addr] : msg.rehomes) space_to_new_host[sid] = addr;

  uint32_t restored = 0;
  uint32_t lost = 0;

  // Collect first, then process — entity_mgr_.ForEach must not observe
  // SetCell / ClearCell mutations mid-walk.
  std::vector<EntityID> to_restore;
  entity_mgr_.ForEach([&](BaseEntity& ent) {
    if (ent.CellAddr() == msg.dead_addr) to_restore.push_back(ent.EntityId());
  });

  for (const EntityID eid : to_restore) {
    auto* ent = entity_mgr_.Find(eid);
    if (ent == nullptr) continue;
    const SpaceID sid = ent->SpaceId();

    auto it = space_to_new_host.find(sid);
    if (it == space_to_new_host.end()) {
      ATLAS_LOG_ERROR(
          "BaseApp: CellAppDeath: no rehome target for entity={} space={} — "
          "Real lost",
          eid, sid);
      ent->ClearCell();
      ++lost;
      continue;
    }
    if (ent->CellBackupData().empty()) {
      ATLAS_LOG_ERROR(
          "BaseApp: CellAppDeath: entity={} has no cached cell_backup_data "
          "(CellApp died before first backup pump tick?) — Real lost",
          eid);
      ent->ClearCell();
      ++lost;
      continue;
    }

    const Address& new_addr = it->second;
    auto ch_result = Network().ConnectRudpNocwnd(new_addr);
    if (!ch_result) {
      ATLAS_LOG_ERROR(
          "BaseApp: CellAppDeath: cannot reach rehome target {}:{} for "
          "entity={} — Real lost",
          new_addr.Ip(), new_addr.Port(), eid);
      ent->ClearCell();
      ++lost;
      continue;
    }
    auto* cell_ch = *ch_result;

    // Clear the stale route so any ClientCellRpc that lands between
    // now and OnCellEntityCreated ack gets dropped cleanly rather
    // than shipped to the dead addr.
    ent->ClearCell();

    cellapp::CreateCellEntity restore;
    restore.base_entity_id = eid;
    restore.type_id = ent->TypeId();
    restore.space_id = sid;
    restore.position = {0.f, 0.f, 0.f};
    restore.direction = {1.f, 0.f, 0.f};
    restore.on_ground = false;
    restore.base_addr = Network().RudpAddress();
    restore.request_id = eid;
    // The restore blob is what BaseApp has been archiving via
    // BackupCellEntity; CellApp's RestoreEntity callback deserialises it.
    restore.script_init_data = ent->CellBackupData();
    (void)cell_ch->SendMessage(restore);
    ++restored;
  }

  ATLAS_LOG_INFO("BaseApp: CellAppDeath complete — restored={} lost={}", restored, lost);
}

void BaseApp::OnCellRpcForward(Channel& /*ch*/, const baseapp::CellRpcForward& msg) {
  // C# entity receives the base-directed RPC call via the native callback table.
  auto dispatch_fn = GetNativeProvider().dispatch_rpc_fn();
  if (!dispatch_fn) {
    ATLAS_LOG_WARNING("BaseApp: OnCellRpcForward: dispatch_rpc callback not registered");
    return;
  }
  dispatch_fn(msg.base_entity_id, msg.rpc_id, reinterpret_cast<const uint8_t*>(msg.payload.data()),
              static_cast<int32_t>(msg.payload.size()));
}

// Path #3 of the three-path CellApp→Client delta contract (see
// delta_forwarder.h for the full contract). SelfRpcFromCell is Reliable, sent
// directly to the owner's client channel with the RPC's own method id. It
// MUST NOT touch DeltaForwarder — that forwarder is latest-wins and would
// silently drop intermediate RPCs.
void BaseApp::OnSelfRpcFromCell(Channel& /*ch*/, const baseapp::SelfRpcFromCell& msg) {
  auto* proxy = entity_mgr_.FindProxy(msg.base_entity_id);
  if (!proxy || !proxy->HasClient()) return;

  if (auto* client_ch = ResolveClientChannel(proxy->EntityId())) {
    (void)client_ch->SendMessage(
        static_cast<MessageID>(msg.rpc_id),
        std::span<const std::byte>(msg.payload.data(), msg.payload.size()));
  }
}

void BaseApp::OnBroadcastRpcFromCell(Channel& /*ch*/, const baseapp::BroadcastRpcFromCell& msg) {
  auto* proxy = entity_mgr_.FindProxy(msg.base_entity_id);
  if (!proxy || !proxy->HasClient()) return;

  if (auto* client_ch = ResolveClientChannel(proxy->EntityId())) {
    (void)client_ch->SendMessage(
        static_cast<MessageID>(msg.rpc_id),
        std::span<const std::byte>(msg.payload.data(), msg.payload.size()));
  }
}

// Path #1 of the three-path CellApp→Client delta contract (see
// delta_forwarder.h for the full contract). ReplicatedDeltaFromCell is
// Unreliable; its payload passes through the per-client DeltaForwarder,
// which enforces LATEST-WINS for the same entity and a per-tick byte budget.
// Use for Volatile position/orientation updates only — NEVER for anything
// that carries event_seq or other cumulative counters, because the forwarder
// will replace pending entries and silently drop intermediate frames.
void BaseApp::OnReplicatedDeltaFromCell(Channel& /*ch*/,
                                        const baseapp::ReplicatedDeltaFromCell& msg) {
  auto* proxy = entity_mgr_.FindProxy(msg.base_entity_id);
  if (!proxy || !proxy->HasClient()) return;

  auto it = entity_client_index_.find(proxy->EntityId());
  if (it == entity_client_index_.end()) return;

  client_delta_forwarders_[it->second].Enqueue(
      msg.base_entity_id, std::span<const std::byte>(msg.delta.data(), msg.delta.size()));
}

// Path #2 of the three-path CellApp→Client delta contract (see
// delta_forwarder.h for the full contract). Reliable deltas carry
// semantically-critical property changes (HP, state, inventory, AoI property
// updates carrying event_seq). They BYPASS DeltaForwarder so the byte-budget
// and same-entity replacement cannot drop them, and ride the reliable
// client-facing message ID so the transport retransmits on loss.
void BaseApp::OnReplicatedReliableDeltaFromCell(
    Channel& /*ch*/, const baseapp::ReplicatedReliableDeltaFromCell& msg) {
  // Reliable deltas carry semantically-critical property changes (HP, state,
  // inventory). They bypass the DeltaForwarder budget so the byte-limit cannot
  // drop them, and they ride the reliable client-facing message ID so the
  // transport retransmits on loss.
  auto* proxy = entity_mgr_.FindProxy(msg.base_entity_id);
  if (!proxy || !proxy->HasClient()) return;

  auto* client_ch = ResolveClientChannel(proxy->EntityId());
  if (!client_ch) return;

  (void)client_ch->SendMessage(DeltaForwarder::kClientReliableDeltaMessageId,
                               std::span<const std::byte>(msg.delta.data(), msg.delta.size()));

  reliable_delta_bytes_sent_total_ += msg.delta.size();
  ++reliable_delta_messages_sent_total_;
}

// Cell-to-base periodic state backup. Base caches the cell-authoritative
// bytes opaquely and reuses them verbatim for DB writes / reviver restart
// / offload migration. See baseapp_messages.h::BackupCellEntity.
void BaseApp::OnBackupCellEntity(const baseapp::BackupCellEntity& msg) {
  auto* entity = entity_mgr_.Find(msg.base_entity_id);
  if (!entity) {
    // Not necessarily an error — entity might have just been destroyed
    // and the last backup is still in flight. Debug-only signal.
    ATLAS_LOG_DEBUG("BaseApp: BackupCellEntity for unknown entity_id={} (dropped)",
                    msg.base_entity_id);
    return;
  }
  // The bytes are opaque — don't touch them. Copy into the entity's
  // cell_backup_data_ slot; the next DB write / reviver / offload
  // consumer will read them out.
  entity->SetCellBackupData(msg.cell_backup_data);
}

// Cell-authoritative baseline relayed to the owning client. Base-side
// SerializeForOwnerClient returns empty blobs for entities whose client-
// visible fields are all cell-scope, so baselines must come from the
// CellApp where the data lives. Relay-only: bytes arrive as cell-side
// SerializeForOwnerClient output and leave as ReplicatedBaselineToClient
// (0xF002) toward the proxy's attached client channel.
void BaseApp::OnReplicatedBaselineFromCell(const baseapp::ReplicatedBaselineFromCell& msg) {
  if (msg.snapshot.empty()) return;  // nothing to ship — skip the no-op round-trip

  auto* proxy = entity_mgr_.FindProxy(msg.base_entity_id);
  if (!proxy || !proxy->HasClient()) return;

  auto* client_ch = ResolveClientChannel(proxy->EntityId());
  if (!client_ch) return;

  baseapp::ReplicatedBaselineToClient out;
  out.base_entity_id = msg.base_entity_id;
  out.snapshot = msg.snapshot;
  (void)client_ch->SendMessage(out);
  baseline_bytes_sent_total_ += static_cast<uint64_t>(msg.snapshot.size());
  ++baseline_messages_sent_total_;
}

void BaseApp::FlushClientDeltas() {
  for (auto it = client_delta_forwarders_.begin(); it != client_delta_forwarders_.end();) {
    auto& [client_addr, forwarder] = *it;

    // Resolve channel; if client disconnected, discard its forwarder.
    auto ent_it = client_entity_index_.find(client_addr);
    if (ent_it == client_entity_index_.end()) {
      it = client_delta_forwarders_.erase(it);
      continue;
    }

    auto* client_ch = ResolveClientChannel(ent_it->second);
    if (!client_ch) {
      it = client_delta_forwarders_.erase(it);
      continue;
    }

    auto bytes = forwarder.Flush(*client_ch, kDeltaBudgetPerTick);
    delta_bytes_sent_total_ += bytes;

    // Remove empty forwarders to avoid map bloat.
    if (forwarder.QueueDepth() == 0)
      it = client_delta_forwarders_.erase(it);
    else
      ++it;
  }
}

void BaseApp::EmitBaselineSnapshots() {
  // Intentional no-op. The baseline pump runs on the CellApp (see
  // CellApp::TickClientBaselinePump + ReplicatedBaselineFromCell msg
  // 2019), which owns authoritative cell-scope data. BaseApp only
  // relays (OnReplicatedBaselineFromCell → ReplicatedBaselineToClient
  // 0xF002). The method stays to keep the stats watcher symbol alive.
  (void)baseline_tick_counter_;  // kept for stats watchers
}

// ============================================================================
// NativeProvider bridging
// ============================================================================

void BaseApp::DoWriteToDb(EntityID entity_id, const std::byte* data, int32_t len) {
  if (!dbapp_channel_) {
    ATLAS_LOG_ERROR("BaseApp: write_to_db: no DBApp connection (entity={})", entity_id);
    return;
  }
  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent) return;

  dbapp::WriteEntity msg;
  // Use kCreateNew for first save (dbid=0), kExplicitDbid for updates
  msg.flags = (ent->Dbid() == kInvalidDBID) ? WriteFlags::kCreateNew : WriteFlags::kExplicitDbid;
  msg.type_id = ent->TypeId();
  msg.dbid = ent->Dbid();
  msg.entity_id = entity_id;
  msg.request_id = entity_id;  // echo back in WriteEntityAck
  msg.blob.assign(data, data + len);
  (void)dbapp_channel_->SendMessage(msg);
}

auto BaseApp::CaptureEntitySnapshot(EntityID entity_id, std::vector<std::byte>& out) -> bool {
  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent) {
    return false;
  }

  // Step 1 — refresh the base-scope blob. GetEntityData callback runs the
  // base-side C# Serialize, which emits DATA_BASE fields only. Cache the
  // result back so live-in-RAM state matches what's about to hit the DB.
  std::vector<std::byte> base_bytes;
  if (HasManagedEntityType(ent->TypeId()) && native_provider_) {
    if (auto fn = native_provider_->get_entity_data_fn()) {
      uint8_t* raw = nullptr;
      int32_t len = 0;
      ClearNativeApiError();
      fn(entity_id, &raw, &len);

      if (auto error = ConsumeNativeApiError()) {
        ATLAS_LOG_ERROR("BaseApp: get_entity_data failed for entity={}: {}", entity_id, *error);
        return false;
      }

      if (len < 0) {
        ATLAS_LOG_ERROR("BaseApp: get_entity_data returned negative length for entity={}",
                        entity_id);
        return false;
      }
      if (len > 0 && raw == nullptr) {
        ATLAS_LOG_ERROR("BaseApp: get_entity_data returned null payload for entity={} len={}",
                        entity_id, len);
        return false;
      }
      if (len > 0) {
        base_bytes.assign(reinterpret_cast<const std::byte*>(raw),
                          reinterpret_cast<const std::byte*>(raw) + len);
      }
      ent->SetEntityData(base_bytes);
    } else {
      // No GetEntityData — fall back to the cached blob so the base
      // portion at least round-trips unchanged.
      base_bytes = ent->EntityData();
      ATLAS_LOG_WARNING("BaseApp: no managed snapshot callback for entity={}, using cached blob",
                        entity_id);
    }
  } else {
    base_bytes = ent->EntityData();
  }

  // Step 2 — stitch in the cell-authoritative blob last pushed up by the
  // CellApp backup pump (see baseapp_messages.h::BackupCellEntity).
  // Entities without a cell side simply get an empty cell span.
  const auto& cell_bytes = ent->CellBackupData();
  out = EncodeDbBlob(std::span<const std::byte>(base_bytes.data(), base_bytes.size()),
                     std::span<const std::byte>(cell_bytes.data(), cell_bytes.size()));
  return true;
}

auto BaseApp::RotateProxySession(EntityID entity_id, const SessionKey& session_key) -> bool {
  auto* proxy = entity_mgr_.FindProxy(entity_id);
  if (!proxy) {
    return false;
  }

  proxy->BumpSessionEpoch();
  return entity_mgr_.AssignSessionKey(entity_id, session_key);
}

void BaseApp::EnterDetachedGrace(EntityID entity_id) {
  auto* proxy = entity_mgr_.FindProxy(entity_id);
  if (!proxy || proxy->Dbid() == kInvalidDBID) {
    return;
  }

  const auto kNow = Clock::now();
  const auto kUntil = kNow + kDetachedProxyGrace;
  proxy->EnterDetachedGrace(kUntil);
  detached_proxies_[entity_id] = DetachedProxyState{kNow, kUntil};
}

void BaseApp::ClearDetachedGrace(EntityID entity_id) {
  detached_proxies_.erase(entity_id);
  if (auto* proxy = entity_mgr_.FindProxy(entity_id)) {
    proxy->ClearDetachedGrace();
  }
}

void BaseApp::ExpireDetachedProxies() {
  const auto kNow = Clock::now();
  for (auto it = detached_proxies_.begin(); it != detached_proxies_.end();) {
    const EntityID kEntityId = it->first;
    auto* proxy = entity_mgr_.FindProxy(kEntityId);
    if (!proxy || proxy->HasClient()) {
      if (proxy) {
        proxy->ClearDetachedGrace();
      }
      it = detached_proxies_.erase(it);
      continue;
    }

    if (kNow < it->second.detached_until) {
      ++it;
      continue;
    }

    proxy->ClearDetachedGrace();
    it = detached_proxies_.erase(it);
    StartDisconnectLogoff(kEntityId);
  }
}

auto BaseApp::DeferredLoginCheckoutCount() const -> std::size_t {
  std::size_t total = 0;
  for (const auto& [entity_id, deferred] : deferred_login_checkouts_) {
    (void)entity_id;
    total += deferred.size();
  }
  return total;
}

auto BaseApp::TryCompleteLocalRelogin(PendingLogin pending) -> bool {
  if (queued_logins_.contains(pending.dbid)) {
    FailPendingPrepareLogin(pending, "superseded_by_newer_login");
    FinishLoginFlow(pending.dbid);
    return true;
  }

  auto* ent = entity_mgr_.FindByDbid(pending.dbid);
  if (!ent || ent->IsPendingDestroy()) {
    return false;
  }

  auto* proxy = entity_mgr_.FindProxy(ent->EntityId());
  if (!proxy) {
    return false;
  }

  const bool kWasDetached = proxy->IsDetached();
  if (auto* existing_client = ResolveClientChannel(proxy->EntityId())) {
    existing_client->Condemn();
  }
  UnbindClient(proxy->EntityId());
  ClearDetachedGrace(proxy->EntityId());

  if (!RotateProxySession(proxy->EntityId(), pending.session_key)) {
    FailPendingPrepareLogin(pending, "session_conflict");
    FinishLoginFlow(pending.dbid);
    return true;
  }

  login::PrepareLoginResult reply;
  reply.request_id = pending.login_request_id;
  reply.success = true;
  reply.entity_id = proxy->EntityId();
  SendPrepareLoginResult(pending.loginapp_addr, reply);

  ++fast_relogin_total_;
  if (kWasDetached) {
    ++detached_relogin_total_;
  }

  ATLAS_LOG_DEBUG("BaseApp: fast relogin entity={} dbid={} detached={} epoch={}", proxy->EntityId(),
                  pending.dbid, kWasDetached ? 1 : 0, proxy->SessionEpoch());
  FinishLoginFlow(pending.dbid);
  return true;
}

void BaseApp::CompletePrepareLoginFromCheckout(PendingLogin pending, DatabaseID dbid,
                                               uint16_t type_id, std::span<const std::byte> blob) {
  login::PrepareLoginResult reply;
  reply.request_id = pending.login_request_id;

  if (pending.reply_sent) {
    ReleaseCheckout(dbid, type_id);
    FinishLoginFlow(pending.dbid);
    return;
  }

  if (queued_logins_.contains(pending.dbid)) {
    ReleaseCheckout(dbid, type_id);
    FailPendingPrepareLogin(pending, "superseded_by_newer_login");
    FinishLoginFlow(pending.dbid);
    return;
  }

  if (auto* blocking_ent = entity_mgr_.FindByDbid(dbid); blocking_ent) {
    DeferPrepareLoginFromCheckout(blocking_ent->EntityId(), std::move(pending), dbid, type_id,
                                  blob);
    StartDisconnectLogoff(blocking_ent->EntityId());
    return;
  }

  auto* ent = entity_mgr_.Create(type_id, true);
  if (!ent) {
    ATLAS_LOG_ERROR("BaseApp: failed to allocate EntityID for login entity type={} dbid={}",
                    type_id, dbid);
    ReleaseCheckout(dbid, type_id);
    reply.success = false;
    reply.error = "entity_id_exhausted";
    SendPrepareLoginResult(pending.loginapp_addr, reply);
    FinishLoginFlow(pending.dbid);
    return;
  }

  // L2: split the DB blob here as well — the login checkout path is the
  // analogue of CreateBaseFromDB. Base bytes go into entity_data_ /
  // Base.Deserialize; cell bytes are parked on the Proxy until
  // CreateCellEntity passes them through script_init_data.
  std::span<const std::byte> base_span, cell_span;
  if (!DecodeDbBlob(blob, base_span, cell_span)) {
    ATLAS_LOG_ERROR("BaseApp: login checkout blob corrupt (dbid={} type={})", dbid, type_id);
    entity_mgr_.Destroy(ent->EntityId());
    ReleaseCheckout(dbid, type_id);
    reply.success = false;
    reply.error = "blob_corrupt";
    SendPrepareLoginResult(pending.loginapp_addr, reply);
    FinishLoginFlow(pending.dbid);
    return;
  }
  ent->SetEntityData(std::vector<std::byte>(base_span.begin(), base_span.end()));
  ent->SetCellBackupData(std::vector<std::byte>(cell_span.begin(), cell_span.end()));
  if (!entity_mgr_.AssignDbid(ent->EntityId(), dbid)) {
    if (auto* blocking_ent = entity_mgr_.FindByDbid(dbid); blocking_ent) {
      entity_mgr_.Destroy(ent->EntityId());
      DeferPrepareLoginFromCheckout(blocking_ent->EntityId(), std::move(pending), dbid, type_id,
                                    blob);
      StartDisconnectLogoff(blocking_ent->EntityId());
      return;
    }

    entity_mgr_.Destroy(ent->EntityId());
    ReleaseCheckout(dbid, type_id);
    reply.success = false;
    reply.error = "dbid_conflict";
    SendPrepareLoginResult(pending.loginapp_addr, reply);
    FinishLoginFlow(pending.dbid);
    return;
  }

  auto* proxy = entity_mgr_.FindProxy(ent->EntityId());
  if (proxy && !RotateProxySession(proxy->EntityId(), pending.session_key)) {
    entity_mgr_.Destroy(ent->EntityId());
    ReleaseCheckout(dbid, type_id);
    reply.success = false;
    reply.error = "session_conflict";
    SendPrepareLoginResult(pending.loginapp_addr, reply);
    FinishLoginFlow(pending.dbid);
    return;
  }

  if (!RestoreManagedEntity(ent->EntityId(), type_id, dbid, base_span)) {
    (void)NotifyManagedEntityDestroyed(ent->EntityId(), "login rollback");
    entity_mgr_.Destroy(ent->EntityId());
    ReleaseCheckout(dbid, type_id);
    reply.success = false;
    reply.error = "restore_entity_failed";
    SendPrepareLoginResult(pending.loginapp_addr, reply);
    FinishLoginFlow(pending.dbid);
    return;
  }

  prepared_login_entities_[pending.login_request_id] =
      PreparedLoginEntity{ent->EntityId(), dbid, type_id, Clock::now()};
  prepared_login_requests_by_entity_[ent->EntityId()] = pending.login_request_id;

  reply.success = true;
  reply.entity_id = ent->EntityId();
  SendPrepareLoginResult(pending.loginapp_addr, reply);
  FinishLoginFlow(pending.dbid);

  ATLAS_LOG_DEBUG("BaseApp: login entity created id={} dbid={}", ent->EntityId(), dbid);
}

void BaseApp::DeferPrepareLoginFromCheckout(EntityID blocking_entity_id, PendingLogin pending,
                                            DatabaseID dbid, uint16_t type_id,
                                            std::span<const std::byte> blob) {
  DeferredLoginCheckout deferred;
  deferred.pending = std::move(pending);
  deferred.dbid = dbid;
  deferred.type_id = type_id;
  deferred.blob.assign(blob.begin(), blob.end());
  deferred_login_checkouts_[blocking_entity_id].push_back(std::move(deferred));
}

void BaseApp::FailDeferredPrepareLogins(EntityID blocking_entity_id, std::string_view reason,
                                        std::vector<DatabaseID>* finished_dbids) {
  auto it = deferred_login_checkouts_.find(blocking_entity_id);
  if (it == deferred_login_checkouts_.end()) {
    return;
  }

  auto deferred = std::move(it->second);
  deferred_login_checkouts_.erase(it);
  std::vector<DatabaseID> local_finished_dbids;
  auto* completion_dbids = finished_dbids != nullptr ? finished_dbids : &local_finished_dbids;
  for (auto& entry : deferred) {
    ReleaseCheckout(entry.dbid, entry.type_id);
    FailPendingPrepareLogin(entry.pending, reason);
    completion_dbids->push_back(entry.pending.dbid);
  }

  if (finished_dbids == nullptr) {
    DrainFinishedLoginFlows(std::move(local_finished_dbids));
  }
}

auto BaseApp::ResumeDeferredPrepareLogins(EntityID blocking_entity_id) -> bool {
  auto it = deferred_login_checkouts_.find(blocking_entity_id);
  if (it == deferred_login_checkouts_.end()) {
    return false;
  }

  auto deferred = std::move(it->second);
  deferred_login_checkouts_.erase(it);
  for (auto& entry : deferred) {
    CompletePrepareLoginFromCheckout(std::move(entry.pending), entry.dbid, entry.type_id,
                                     entry.blob);
  }
  return !deferred.empty();
}

void BaseApp::SubmitPrepareLogin(PendingLogin pending) {
  if (!active_login_dbids_.insert(pending.dbid).second) {
    auto queued_it = queued_logins_.find(pending.dbid);
    if (queued_it != queued_logins_.end()) {
      FailPendingPrepareLogin(queued_it->second, "superseded_by_newer_login");
    }
    queued_logins_[pending.dbid] = std::move(pending);
    return;
  }

  DispatchPrepareLogin(std::move(pending));
}

void BaseApp::DispatchPrepareLogin(PendingLogin pending) {
  if (TryCompleteLocalRelogin(pending)) {
    return;
  }

  const bool kAlreadyOnline = entity_mgr_.FindByDbid(pending.dbid) != nullptr;
  if (kAlreadyOnline) {
    const uint32_t kRid = next_prepare_request_id_++;
    pending_force_logoffs_[kRid] = std::move(pending);

    baseapp::ForceLogoff fo;
    fo.dbid = pending_force_logoffs_[kRid].dbid;
    fo.request_id = kRid;
    ProcessForceLogoffRequest(fo);
    return;
  }

  // If LoginApp already checked out and provided the blob, skip DBApp round-trip
  if (pending.blob_prefetched) {
    const DatabaseID kDbid = pending.dbid;
    const uint16_t kTypeId = pending.type_id;
    auto blob = std::move(pending.entity_blob);
    CompletePrepareLoginFromCheckout(std::move(pending), kDbid, kTypeId,
                                     std::span<const std::byte>(blob));
    return;
  }

  const uint32_t kRid = next_prepare_request_id_++;
  const DatabaseID kDbid = pending.dbid;
  const uint16_t kTypeId = pending.type_id;
  pending_logins_[kRid] = std::move(pending);

  if (!dbapp_channel_) {
    ATLAS_LOG_ERROR("BaseApp: PrepareLogin: no DBApp connection");
    FailPendingPrepareLogin(kRid, "no_dbapp");
    FinishLoginFlow(kDbid);
    return;
  }

  dbapp::CheckoutEntity co;
  co.request_id = kRid;
  co.dbid = kDbid;
  co.type_id = kTypeId;
  auto send_result = dbapp_channel_->SendMessage(co);
  if (!send_result) {
    FailPendingPrepareLogin(kRid, "checkout_send_failed");
    FinishLoginFlow(kDbid);
  }
}

void BaseApp::FinishLoginFlow(DatabaseID dbid) {
  if (dbid == kInvalidDBID) {
    return;
  }

  active_login_dbids_.erase(dbid);

  auto queued_it = queued_logins_.find(dbid);
  if (queued_it == queued_logins_.end()) {
    return;
  }

  PendingLogin next = std::move(queued_it->second);
  queued_logins_.erase(queued_it);

  active_login_dbids_.insert(dbid);
  DispatchPrepareLogin(std::move(next));
}

void BaseApp::DrainFinishedLoginFlows(std::vector<DatabaseID> dbids) {
  dbids.erase(std::remove(dbids.begin(), dbids.end(), kInvalidDBID), dbids.end());
  if (dbids.empty()) {
    return;
  }

  std::sort(dbids.begin(), dbids.end());
  dbids.erase(std::unique(dbids.begin(), dbids.end()), dbids.end());
  for (DatabaseID dbid : dbids) {
    FinishLoginFlow(dbid);
  }
}

void BaseApp::FlushRemoteForceLogoffAcks(EntityID entity_id, bool success) {
  auto it = pending_remote_force_logoff_acks_.find(entity_id);
  if (it == pending_remote_force_logoff_acks_.end()) {
    return;
  }

  for (const PendingRemoteForceLogoffAck& pending_ack : it->second) {
    if (auto* reply_ch = ResolveInternalChannel(pending_ack.reply_addr)) {
      baseapp::ForceLogoffAck ack;
      ack.request_id = pending_ack.request_id;
      ack.success = success;
      (void)reply_ch->SendMessage(ack);
    }
  }

  pending_remote_force_logoff_acks_.erase(it);
}

void BaseApp::FlushAllRemoteForceLogoffAcks(bool success) {
  auto pending_acks = std::move(pending_remote_force_logoff_acks_);
  pending_remote_force_logoff_acks_.clear();

  for (const auto& [entity_id, queued] : pending_acks) {
    (void)entity_id;
    for (const PendingRemoteForceLogoffAck& pending_ack : queued) {
      if (auto* reply_ch = ResolveInternalChannel(pending_ack.reply_addr)) {
        baseapp::ForceLogoffAck ack;
        ack.request_id = pending_ack.request_id;
        ack.success = success;
        (void)reply_ch->SendMessage(ack);
      }
    }
  }
}

void BaseApp::BeginLogoffPersist(EntityID entity_id, DatabaseID dbid, uint16_t type_id,
                                 uint32_t continuation_request_id) {
  if (entity_id == kInvalidEntityID) {
    if (continuation_request_id != 0)
      ContinueLoginAfterForceLogoff(continuation_request_id);
    else
      FinishLoginFlow(dbid);
    return;
  }

  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent || ent->Dbid() != dbid || ent->IsPendingDestroy()) {
    if (continuation_request_id != 0)
      ContinueLoginAfterForceLogoff(continuation_request_id);
    else
      FinishLoginFlow(dbid);
    return;
  }

  std::vector<std::byte> blob;
  if (!CaptureEntitySnapshot(entity_id, blob)) {
    if (continuation_request_id != 0)
      FailPendingForceLogoff(continuation_request_id, "force_logoff_snapshot_failed");
    FlushRemoteForceLogoffAcks(entity_id, false);
    FinishLoginFlow(dbid);
    return;
  }

  // Optimistic checkin: release the DB-side checkout immediately so that a
  // subsequent CheckoutEntity for the same DBID can succeed without waiting
  // for the data write to complete.
  ReleaseCheckout(dbid, type_id);

  // Fire-and-forget data persistence — the checkout is already released so
  // we don't use WriteFlags::LogOff.  The WriteEntityAck for this request
  // will be silently ignored (entity already destroyed locally).
  if (dbapp_channel_) {
    dbapp::WriteEntity msg;
    msg.flags = WriteFlags::kExplicitDbid;
    msg.type_id = type_id;
    msg.dbid = dbid;
    msg.entity_id = entity_id;
    msg.request_id = next_prepare_request_id_++;
    msg.blob = std::move(blob);
    (void)dbapp_channel_->SendMessage(msg);
  }

  // Destroy the local entity immediately.
  if (!FinalizeForceLogoff(entity_id)) {
    if (continuation_request_id != 0)
      FailPendingForceLogoff(continuation_request_id, "force_logoff_destroy_failed");
    if (auto waiter_it = pending_local_force_logoff_waiters_.find(entity_id);
        waiter_it != pending_local_force_logoff_waiters_.end()) {
      for (uint32_t waiter_request_id : waiter_it->second)
        FailPendingForceLogoff(waiter_request_id, "force_logoff_destroy_failed");
      pending_local_force_logoff_waiters_.erase(waiter_it);
    }
    FailDeferredPrepareLogins(entity_id, "force_logoff_destroy_failed");
    FlushRemoteForceLogoffAcks(entity_id, false);
    FinishLoginFlow(dbid);
    return;
  }

  // All post-logoff continuations are processed synchronously — no need to
  // wait for WriteEntityAck.
  const bool kResumedDeferred = ResumeDeferredPrepareLogins(entity_id);

  if (auto waiter_it = pending_local_force_logoff_waiters_.find(entity_id);
      waiter_it != pending_local_force_logoff_waiters_.end()) {
    for (uint32_t waiter_request_id : waiter_it->second) {
      if (pending_force_logoffs_.contains(waiter_request_id))
        ContinueLoginAfterForceLogoff(waiter_request_id);
    }
    pending_local_force_logoff_waiters_.erase(waiter_it);
  }

  FlushRemoteForceLogoffAcks(entity_id, true);

  if (continuation_request_id != 0 && pending_force_logoffs_.contains(continuation_request_id)) {
    ContinueLoginAfterForceLogoff(continuation_request_id);
  } else if (!kResumedDeferred) {
    FinishLoginFlow(dbid);
  }
}

void BaseApp::BeginForceLogoffPersist(uint32_t force_request_id, EntityID entity_id) {
  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent || ent->Dbid() == kInvalidDBID) {
    if (!FinalizeForceLogoff(entity_id)) {
      FailPendingForceLogoff(force_request_id, "force_logoff_destroy_failed");
      auto pending_it = pending_force_logoffs_.find(force_request_id);
      if (pending_it != pending_force_logoffs_.end()) {
        FinishLoginFlow(pending_it->second.dbid);
      }
      return;
    }
    ContinueLoginAfterForceLogoff(force_request_id);
    return;
  }

  BeginLogoffPersist(entity_id, ent->Dbid(), ent->TypeId(), force_request_id);
}

void BaseApp::StartDisconnectLogoff(EntityID entity_id) {
  ClearDetachedGrace(entity_id);

  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent || ent->IsPendingDestroy()) {
    return;
  }

  const DatabaseID kDbid = ent->Dbid();
  if (kDbid != kInvalidDBID) {
    active_login_dbids_.insert(kDbid);
  }

  if (kDbid == kInvalidDBID) {
    (void)FinalizeForceLogoff(entity_id);
    return;
  }

  BeginLogoffPersist(entity_id, kDbid, ent->TypeId(), 0);
}

void BaseApp::ContinueLoginAfterForceLogoff(uint32_t force_request_id) {
  auto it = pending_force_logoffs_.find(force_request_id);
  if (it == pending_force_logoffs_.end()) {
    return;
  }

  PendingLogin pending = it->second;
  pending_force_logoffs_.erase(it);

  if (pending.reply_sent) {
    return;
  }

  if (!dbapp_channel_) {
    FailPendingPrepareLogin(pending, "no_dbapp");
    FinishLoginFlow(pending.dbid);
    return;
  }

  uint32_t new_rid = next_prepare_request_id_++;
  pending_logins_[new_rid] = pending;

  dbapp::CheckoutEntity co;
  co.request_id = new_rid;
  co.dbid = pending.dbid;
  co.type_id = pending.type_id;
  auto send_result = dbapp_channel_->SendMessage(co);
  if (!send_result) {
    pending_logins_.erase(new_rid);
    FailPendingPrepareLogin(pending, "checkout_send_failed");
    FinishLoginFlow(pending.dbid);
  }
}

auto BaseApp::FinalizeForceLogoff(EntityID entity_id) -> bool {
  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent) {
    return true;
  }

  // If the entity has a cell counterpart, tell the owning CellApp to
  // destroy it before we drop the base side. Without this the cell
  // entity leaks forever — it has no independent destroy trigger.
  // Covers the shortline / script-created path (StressAvatar et al.);
  // DB-backed entities that go through BeginLogoffPersist also want
  // this hook but can share the same helper call when that path lands.
  if (ent->HasCell()) {
    if (auto* cell_ch = ResolveCellChannelForEntity(entity_id)) {
      cellapp::DestroyCellEntity msg;
      msg.base_entity_id = entity_id;
      (void)cell_ch->SendMessage(msg);
      ATLAS_LOG_DEBUG("BaseApp: sent DestroyCellEntity for entity={}", entity_id);
    }
    ent->ClearCell();
  }

  if (!NotifyManagedEntityDestroyed(entity_id, "force logoff")) {
    return false;
  }

  ClearDetachedGrace(entity_id);
  UnbindClient(entity_id);
  (void)entity_mgr_.ClearSessionKey(entity_id);
  (void)entity_mgr_.AssignDbid(entity_id, kInvalidDBID);
  ent->MarkForDestroy();
  return true;
}

void BaseApp::ProcessForceLogoffRequest(const baseapp::ForceLogoff& msg) {
  EntityID found_id = kInvalidEntityID;
  if (auto* ent = entity_mgr_.FindByDbid(msg.dbid)) {
    found_id = ent->EntityId();
  }

  if (found_id != kInvalidEntityID) {
    if (auto* found_ent = entity_mgr_.Find(found_id); found_ent && found_ent->IsPendingDestroy()) {
      if (pending_force_logoffs_.contains(msg.request_id))
        ContinueLoginAfterForceLogoff(msg.request_id);
      return;
    }

    ++force_logoff_total_;
    ATLAS_LOG_DEBUG("BaseApp: ForceLogoff: processing entity={} dbid={}", found_id, msg.dbid);
    if (pending_force_logoffs_.contains(msg.request_id)) {
      BeginForceLogoffPersist(msg.request_id, found_id);
      return;
    }

    (void)FinalizeForceLogoff(found_id);
  }

  if (pending_force_logoffs_.contains(msg.request_id)) {
    ContinueLoginAfterForceLogoff(msg.request_id);
  }
}

void BaseApp::DoGiveClientToLocal(EntityID src_id, EntityID dest_id) {
  auto* src_proxy = entity_mgr_.FindProxy(src_id);
  auto* dst_proxy = entity_mgr_.FindProxy(dest_id);
  if (!src_proxy || !dst_proxy) {
    ATLAS_LOG_ERROR("BaseApp: give_client_to: invalid src={} or dest={}", src_id, dest_id);
    return;
  }
  if (!src_proxy->HasClient()) {
    ATLAS_LOG_WARNING("BaseApp: give_client_to: src={} has no client", src_id);
    return;
  }
  const auto kSessionKey = src_proxy->GetSessionKey();
  (void)entity_mgr_.ClearSessionKey(src_id);
  if (!entity_mgr_.AssignSessionKey(dest_id, kSessionKey)) {
    ATLAS_LOG_ERROR("BaseApp: give_client_to: failed to move session key to dest={}", dest_id);
    return;
  }

  const auto kClientAddr = src_proxy->ClientAddr();
  UnbindClient(src_id);
  if (!BindClient(dest_id, kClientAddr)) {
    ATLAS_LOG_ERROR("BaseApp: give_client_to: failed to move client binding to dest={}", dest_id);
    return;
  }

  // Tell the client its owning entity has changed. Without this message
  // the client still thinks `src_id` is its entity (from AuthenticateResult),
  // so every subsequent ClientCellRpc would target a detached Proxy at
  // validation time.
  if (auto* client_ch = ResolveClientChannel(dest_id)) {
    baseapp::EntityTransferred notify;
    notify.new_entity_id = dest_id;
    notify.new_type_id = dst_proxy->TypeId();
    (void)client_ch->SendMessage(notify);
  }
}

auto BaseApp::CreateBaseEntityFromScript(uint16_t type_id, SpaceID space_id) -> EntityID {
  const auto& defs = EntityDefs();
  auto* type = defs.FindById(type_id);
  if (!type) {
    ATLAS_LOG_ERROR("BaseApp: CreateBaseEntityFromScript: unknown type_id {}", type_id);
    return 0;
  }

  auto* ent = entity_mgr_.Create(type_id, type->has_client);
  if (!ent) {
    ATLAS_LOG_ERROR("BaseApp: CreateBaseEntityFromScript: EntityID pool exhausted for type_id {}",
                    type_id);
    return 0;
  }
  const EntityID kEid = ent->EntityId();

  // Instantiate the C# script-side via RestoreEntity with an empty blob
  // (script defaults). Same path the login flow uses after DBApp checkout.
  if (!RestoreManagedEntity(kEid, type_id, kInvalidDBID, {})) {
    ATLAS_LOG_ERROR("BaseApp: CreateBaseEntityFromScript: RestoreManagedEntity failed for {}",
                    kEid);
    entity_mgr_.Destroy(kEid);
    return 0;
  }

  // For cell-bearing types, materialise a cell counterpart on a CellApp.
  // Pick the CellApp deterministically by space_id over a stably-sorted
  // peer list: every entity in the same space must land on the same
  // CellApp, otherwise a second CreateCellEntity arriving at a different
  // CellApp would auto-create a disconnected second copy of the space.
  // Keyed by Address so ordering is independent of Birth arrival order.
  // TODO: replace with a CellAppMgr-routed, load-aware allocation.
  if (type->has_cell) {
    const auto& peers = cellapp_peers_.Channels();
    if (peers.empty()) {
      ATLAS_LOG_WARNING(
          "BaseApp: CreateBaseEntityFromScript: type {} has_cell but no CellApp peer available",
          type_id);
    } else {
      std::vector<std::pair<Address, Channel*>> sorted_peers(peers.begin(), peers.end());
      std::sort(sorted_peers.begin(), sorted_peers.end(), [](const auto& a, const auto& b) {
        if (a.first.Ip() != b.first.Ip()) return a.first.Ip() < b.first.Ip();
        return a.first.Port() < b.first.Port();
      });
      const SpaceID effective_space_id = space_id == kInvalidSpaceID ? SpaceID{1} : space_id;
      // Stamp the space_id on the BaseEntity so a later CellApp-death
      // restore can look up the new host keyed by {dead_addr, space_id}.
      // Done unconditionally — if the send below fails, the entity has
      // no cell to restore anyway.
      ent->SetSpaceId(effective_space_id);
      const std::size_t cell_index =
          static_cast<std::size_t>(effective_space_id - 1) % sorted_peers.size();
      auto* cell_ch = sorted_peers[cell_index].second;
      cellapp::CreateCellEntity msg;
      msg.base_entity_id = kEid;
      msg.type_id = type_id;
      msg.space_id = effective_space_id;
      msg.position = {0.f, 0.f, 0.f};
      msg.direction = {1.f, 0.f, 0.f};
      msg.on_ground = false;
      msg.base_addr = Network().RudpAddress();
      msg.request_id = kEid;
      // If the Proxy holds cell_backup_data_ from a prior DB checkout
      // or cell-side backup push, hand it to the cell via script_init_data
      // so Cell.Deserialize can hydrate cell-scope properties. Empty on
      // first-time CreateBaseEntityFromScript — cell uses type defaults.
      if (auto* ent_new = entity_mgr_.Find(kEid); ent_new) {
        msg.script_init_data = ent_new->CellBackupData();
      }
      (void)cell_ch->SendMessage(msg);
      ATLAS_LOG_INFO("BaseApp: sent CreateCellEntity for entity={} type={} space={} to {}", kEid,
                     type_id, effective_space_id, sorted_peers[cell_index].first.ToString());
    }
  }

  return kEid;
}

void BaseApp::DoGiveClientToRemote(EntityID src_id, EntityID /*dest_id*/,
                                   const Address& dest_baseapp) {
  auto* src_proxy = entity_mgr_.FindProxy(src_id);
  if (!src_proxy || !src_proxy->HasClient()) {
    ATLAS_LOG_ERROR("BaseApp: give_client_to_remote: src={} has no client", src_id);
    return;
  }
  // Send AcceptClient to the target BaseApp (its internal RUDP address)
  auto dest_ch_result = Network().ConnectRudpNocwnd(dest_baseapp);
  if (!dest_ch_result) {
    ATLAS_LOG_ERROR("BaseApp: give_client_to_remote: could not connect to {}:{}", dest_baseapp.Ip(),
                    dest_baseapp.Port());
    return;
  }

  baseapp::AcceptClient accept;
  accept.dest_entity_id = src_id;
  accept.session_key = src_proxy->GetSessionKey();
  (void)(*dest_ch_result)->SendMessage(accept);

  UnbindClient(src_id);
}

// ============================================================================
// Login flow handlers
// ============================================================================

void BaseApp::OnRegisterBaseappAck(Channel& /*ch*/, const baseappmgr::RegisterBaseAppAck& msg) {
  if (!msg.success) {
    ATLAS_LOG_ERROR("BaseApp: RegisterBaseAppAck failed — shutting down");
    Shutdown();
    return;
  }
  app_id_ = msg.app_id;

  // Request initial batch of EntityIDs from DBApp
  MaybeRequestMoreIds();

  // Notify BaseAppMgr that we are ready
  if (baseappmgr_channel_) {
    baseappmgr::BaseAppReady ready;
    ready.app_id = app_id_;
    (void)baseappmgr_channel_->SendMessage(ready);
  }

  ATLAS_LOG_INFO("BaseApp: registered as app_id={}", app_id_);
}

void BaseApp::OnGetEntityIdsAck(Channel& /*ch*/, const dbapp::GetEntityIdsAck& msg) {
  id_client_.AddIds(msg.start, msg.end);
  ATLAS_LOG_INFO("BaseApp: received {} EntityIDs [{},{}] from DBApp, available={}", msg.count,
                 msg.start, msg.end, id_client_.Available());
}

auto BaseApp::ResolveInternalChannel(const Address& addr) -> Channel* {
  if (auto* existing = Network().FindChannel(addr)) {
    return existing;
  }

  auto result = Network().ConnectRudpNocwnd(addr);
  if (!result) {
    ATLAS_LOG_WARNING("BaseApp: failed to resolve internal channel {}:{}", addr.Ip(), addr.Port());
    return nullptr;
  }

  return static_cast<Channel*>(*result);
}

auto BaseApp::ResolveClientChannel(EntityID entity_id) -> Channel* {
  auto it = entity_client_index_.find(entity_id);
  if (it == entity_client_index_.end()) {
    return nullptr;
  }

  auto* proxy = entity_mgr_.FindProxy(entity_id);
  if (!proxy || !proxy->HasClient()) {
    auto reverse = client_entity_index_.find(it->second);
    if (reverse != client_entity_index_.end() && reverse->second == entity_id) {
      client_entity_index_.erase(reverse);
    }
    entity_client_index_.erase(it);
    return nullptr;
  }

  auto* ch = external_network_.FindChannel(it->second);
  if (!ch) {
    UnbindClient(entity_id);
    return nullptr;
  }

  return ch;
}

auto BaseApp::BindClient(EntityID entity_id, const Address& client_addr) -> bool {
  auto* proxy = entity_mgr_.FindProxy(entity_id);
  if (!proxy) {
    return false;
  }

  auto existing_client = client_entity_index_.find(client_addr);
  if (existing_client != client_entity_index_.end() && existing_client->second != entity_id) {
    UnbindClient(existing_client->second);
  }

  auto existing_entity = entity_client_index_.find(entity_id);
  if (existing_entity != entity_client_index_.end() && existing_entity->second != client_addr) {
    client_entity_index_.erase(existing_entity->second);
  }

  ClearDetachedGrace(entity_id);
  entity_client_index_[entity_id] = client_addr;
  client_entity_index_[client_addr] = entity_id;
  proxy->BindClient(client_addr);

  // Tell the cell to attach a witness now that this entity has a client.
  // If the cell ack (OnCellEntityCreated) hasn't landed yet — HasCell() is
  // false — the EnableWitness is emitted by the ack handler instead (see
  // OnCellEntityCreated race fix).
  if (proxy->HasCell()) {
    if (auto* cell_ch = ResolveCellChannelForEntity(entity_id)) {
      cellapp::EnableWitness ew;
      ew.base_entity_id = entity_id;
      (void)cell_ch->SendMessage(ew);
    }
  }
  return true;
}

void BaseApp::UnbindClient(EntityID entity_id) {
  auto it = entity_client_index_.find(entity_id);
  if (it != entity_client_index_.end()) {
    auto reverse = client_entity_index_.find(it->second);
    if (reverse != client_entity_index_.end() && reverse->second == entity_id) {
      client_entity_index_.erase(reverse);
    }
    entity_client_index_.erase(it);
  }

  if (auto* proxy = entity_mgr_.FindProxy(entity_id)) {
    // Symmetric to BindClient: tell the cell to drop the witness before
    // the BaseApp-side proxy sheds the client binding. Idempotent on the
    // cell side — OnDisableWitness is a no-op when no witness is attached,
    // which covers the "client disconnected before cell ack" ordering.
    if (proxy->HasCell()) {
      if (auto* cell_ch = ResolveCellChannelForEntity(entity_id)) {
        cellapp::DisableWitness dw;
        dw.base_entity_id = entity_id;
        (void)cell_ch->SendMessage(dw);
      }
    }
    proxy->UnbindClient();
  }
}

void BaseApp::OnExternalClientDisconnect(Channel& ch) {
  auto it = client_entity_index_.find(ch.RemoteAddress());
  if (it == client_entity_index_.end()) {
    return;
  }

  const auto kEntityId = it->second;
  UnbindClient(kEntityId);
  if (auto* proxy = entity_mgr_.FindProxy(kEntityId); proxy && proxy->Dbid() != kInvalidDBID) {
    EnterDetachedGrace(kEntityId);
    ATLAS_LOG_DEBUG("BaseApp: client disconnected from entity={} entering detached grace",
                    kEntityId);
    return;
  }

  StartDisconnectLogoff(kEntityId);
  ATLAS_LOG_DEBUG("BaseApp: client disconnected from entity={}", kEntityId);
}

void BaseApp::SendPrepareLoginResult(const Address& reply_addr,
                                     const login::PrepareLoginResult& msg) {
  if (auto* ch = this->ResolveInternalChannel(reply_addr)) {
    (void)ch->SendMessage(msg);
  }
}

void BaseApp::CleanupExpiredPendingRequests() {
  const auto kNow = Clock::now();
  struct ExpiredLoginRequest {
    uint32_t request_id{0};
    PendingLogin pending;
  };
  std::vector<ExpiredLoginRequest> expired_login_requests;
  std::vector<uint32_t> expired_force_logoff_requests;
  std::vector<uint32_t> stalled_force_logoffs;
  std::vector<uint32_t> expired_prepared_login_requests;
  std::vector<DatabaseID> finished_login_dbids;
  std::unordered_set<uint32_t> expired_force_logoff_request_ids;

  std::erase_if(canceled_login_checkouts_, [kNow](const auto& entry) {
    return kNow - entry.second.canceled_at > kCanceledCheckoutRetention;
  });

  for (auto& [request_id, pending] : pending_logins_) {
    if (!pending.reply_sent && kNow - pending.created_at > kPendingTimeout) {
      ATLAS_LOG_WARNING("BaseApp: prepare-login request_id={} timed out", request_id);
      FailPendingPrepareLogin(pending, "timeout");
      finished_login_dbids.push_back(pending.dbid);
      expired_login_requests.push_back(ExpiredLoginRequest{request_id, pending});
    }
  }

  for (auto& [request_id, pending] : pending_force_logoffs_) {
    if (!pending.reply_sent && kNow - pending.created_at > kPendingTimeout) {
      ATLAS_LOG_WARNING("BaseApp: force-logoff request_id={} timed out", request_id);
      FailPendingForceLogoff(pending, "timeout");
      finished_login_dbids.push_back(pending.dbid);
      expired_force_logoff_requests.push_back(request_id);
      expired_force_logoff_request_ids.insert(request_id);
    } else if (pending.next_force_logoff_retry_at != TimePoint{} &&
               kNow >= pending.next_force_logoff_retry_at) {
      stalled_force_logoffs.push_back(request_id);
    }
  }

  for (auto it = pending_logoff_writes_.begin(); it != pending_logoff_writes_.end();) {
    const bool kTimedOut = kNow - it->second.created_at > kPendingTimeout;
    const bool kOrphaned =
        it->second.continuation_request_id != 0 &&
        (!pending_force_logoffs_.contains(it->second.continuation_request_id) ||
         expired_force_logoff_request_ids.contains(it->second.continuation_request_id));
    if (kTimedOut || kOrphaned) {
      if (kTimedOut) {
        ATLAS_LOG_WARNING("BaseApp: logoff write request_id={} entity_id={} timed out", it->first,
                          it->second.entity_id);
      }

      logoff_entities_in_flight_.erase(it->second.entity_id);
      FailDeferredPrepareLogins(it->second.entity_id, "timeout", &finished_login_dbids);
      if (auto waiter_it = pending_local_force_logoff_waiters_.find(it->second.entity_id);
          waiter_it != pending_local_force_logoff_waiters_.end()) {
        for (uint32_t waiter_request_id : waiter_it->second) {
          FailPendingForceLogoff(waiter_request_id, "timeout");
        }
        pending_local_force_logoff_waiters_.erase(waiter_it);
      }
      FlushRemoteForceLogoffAcks(it->second.entity_id, false);
      finished_login_dbids.push_back(it->second.dbid);
      it = pending_logoff_writes_.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = deferred_login_checkouts_.begin(); it != deferred_login_checkouts_.end();) {
    auto& deferred = it->second;
    for (auto entry_it = deferred.begin(); entry_it != deferred.end();) {
      if (!entry_it->pending.reply_sent && kNow - entry_it->pending.created_at > kPendingTimeout) {
        ReleaseCheckout(entry_it->dbid, entry_it->type_id);
        FailPendingPrepareLogin(entry_it->pending, "timeout");
        finished_login_dbids.push_back(entry_it->pending.dbid);
        entry_it = deferred.erase(entry_it);
      } else {
        ++entry_it;
      }
    }

    if (deferred.empty()) {
      it = deferred_login_checkouts_.erase(it);
    } else {
      ++it;
    }
  }

  for (auto it = queued_logins_.begin(); it != queued_logins_.end();) {
    if (!it->second.reply_sent && kNow - it->second.created_at > kPendingTimeout) {
      ATLAS_LOG_WARNING("BaseApp: queued prepare-login dbid={} timed out", it->first);
      FailPendingPrepareLogin(it->second, "timeout");
      it = queued_logins_.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto& expired : expired_login_requests) {
    CancelInflightCheckout(expired.request_id, expired.pending);
    pending_logins_.erase(expired.request_id);
  }

  for (uint32_t request_id : expired_force_logoff_requests) {
    pending_force_logoffs_.erase(request_id);
  }

  for (uint32_t request_id : stalled_force_logoffs) {
    RetryStalledForceLogoff(request_id);
  }

  for (const auto& [login_request_id, prepared] : prepared_login_entities_) {
    if (kNow - prepared.prepared_at > kPreparedLoginTimeout) {
      expired_prepared_login_requests.push_back(login_request_id);
    }
  }

  for (uint32_t login_request_id : expired_prepared_login_requests) {
    ATLAS_LOG_WARNING(
        "BaseApp: prepared login request_id={} expired before client "
        "authenticate",
        login_request_id);
    ++prepared_login_timeout_total_;
    (void)RollbackPreparedLoginEntity(login_request_id);
  }

  DrainFinishedLoginFlows(std::move(finished_login_dbids));
}

void BaseApp::FailAllDbappPendingRequests(std::string_view reason) {
  for (auto& [request_id, pending] : pending_logins_) {
    (void)request_id;
    FailPendingPrepareLogin(pending, reason);
  }
  pending_logins_.clear();

  for (auto& [request_id, pending] : pending_force_logoffs_) {
    (void)request_id;
    FailPendingForceLogoff(pending, reason);
  }
  pending_force_logoffs_.clear();
  pending_logoff_writes_.clear();
  canceled_login_checkouts_.clear();
  FlushAllRemoteForceLogoffAcks(false);
  for (auto& [entity_id, deferred] : deferred_login_checkouts_) {
    (void)entity_id;
    for (auto& entry : deferred) {
      FailPendingPrepareLogin(entry.pending, reason);
    }
  }
  deferred_login_checkouts_.clear();
  pending_local_force_logoff_waiters_.clear();
  logoff_entities_in_flight_.clear();

  for (auto& [dbid, pending] : queued_logins_) {
    (void)dbid;
    FailPendingPrepareLogin(pending, reason);
  }
  queued_logins_.clear();
  active_login_dbids_.clear();
}

void BaseApp::FailPendingPrepareLogin(PendingLogin& pending, std::string_view reason) {
  if (pending.reply_sent) {
    return;
  }

  login::PrepareLoginResult reply;
  reply.request_id = pending.login_request_id;
  reply.success = false;
  reply.error = std::string(reason);
  SendPrepareLoginResult(pending.loginapp_addr, reply);
  pending.reply_sent = true;
}

void BaseApp::FailPendingPrepareLogin(uint32_t request_id, std::string_view reason) {
  auto it = pending_logins_.find(request_id);
  if (it == pending_logins_.end()) {
    return;
  }
  FailPendingPrepareLogin(it->second, reason);
  pending_logins_.erase(it);
}

void BaseApp::FailPendingForceLogoff(PendingLogin& pending, std::string_view reason) {
  if (pending.reply_sent) {
    return;
  }

  login::PrepareLoginResult reply;
  reply.request_id = pending.login_request_id;
  reply.success = false;
  reply.error = std::string(reason);
  SendPrepareLoginResult(pending.loginapp_addr, reply);
  pending.reply_sent = true;
}

void BaseApp::FailPendingForceLogoff(uint32_t request_id, std::string_view reason) {
  auto it = pending_force_logoffs_.find(request_id);
  if (it == pending_force_logoffs_.end()) {
    return;
  }
  FailPendingForceLogoff(it->second, reason);
  pending_force_logoffs_.erase(it);
}

void BaseApp::ScheduleForceLogoffRetry(PendingLogin& pending, TimePoint now) {
  const auto kShift = std::min<uint8_t>(pending.force_logoff_retry_count, 3);
  const auto kDelay =
      std::min(kForceLogoffRetryBaseDelay * (1 << kShift), kForceLogoffRetryMaxDelay);
  pending.next_force_logoff_retry_at = now + kDelay;
  if (pending.force_logoff_retry_count < std::numeric_limits<uint8_t>::max()) {
    ++pending.force_logoff_retry_count;
  }
}

void BaseApp::RetryStalledForceLogoff(uint32_t request_id) {
  auto it = pending_force_logoffs_.find(request_id);
  if (it == pending_force_logoffs_.end()) {
    return;
  }

  PendingLogin& pending = it->second;
  if (!pending.waiting_for_remote_force_logoff_ack ||
      pending.force_logoff_holder_addr.Port() == 0 ||
      pending.force_logoff_holder_addr == Network().RudpAddress()) {
    pending.next_force_logoff_retry_at = {};
    pending.waiting_for_remote_force_logoff_ack = false;
    return;
  }

  ScheduleForceLogoffRetry(pending, Clock::now());
  if (auto* holder_ch = ResolveInternalChannel(pending.force_logoff_holder_addr)) {
    baseapp::ForceLogoff force_logoff;
    force_logoff.dbid = pending.dbid;
    force_logoff.request_id = request_id;
    (void)holder_ch->SendMessage(force_logoff);
  }
}

void BaseApp::ReleaseCheckout(DatabaseID dbid, uint16_t type_id) {
  if (!dbapp_channel_ || dbid == kInvalidDBID) {
    return;
  }

  dbapp::CheckinEntity checkin;
  checkin.type_id = type_id;
  checkin.dbid = dbid;
  (void)dbapp_channel_->SendMessage(checkin);
}

void BaseApp::CancelInflightCheckout(uint32_t request_id, const PendingLogin& pending) {
  canceled_login_checkouts_[request_id] =
      CanceledCheckout{pending.dbid, pending.type_id, Clock::now()};
  ++canceled_checkout_total_;
  SendAbortCheckout(request_id, pending.dbid, pending.type_id);
}

void BaseApp::SendAbortCheckout(uint32_t request_id, DatabaseID dbid, uint16_t type_id) {
  if (!dbapp_channel_) {
    ATLAS_LOG_WARNING(
        "BaseApp: cannot abort checkout request_id={} dbid={} without DBApp "
        "channel",
        request_id, dbid);
    return;
  }

  dbapp::AbortCheckout abort;
  abort.request_id = request_id;
  abort.type_id = type_id;
  abort.dbid = dbid;
  (void)dbapp_channel_->SendMessage(abort);
}

auto BaseApp::RollbackPreparedLoginEntity(uint32_t login_request_id) -> bool {
  auto it = prepared_login_entities_.find(login_request_id);
  if (it == prepared_login_entities_.end()) {
    return false;
  }

  const PreparedLoginEntity kPrepared = it->second;
  prepared_login_entities_.erase(it);
  prepared_login_requests_by_entity_.erase(kPrepared.entity_id);

  auto* ent = entity_mgr_.Find(kPrepared.entity_id);
  if (!ent) {
    return true;
  }

  (void)NotifyManagedEntityDestroyed(kPrepared.entity_id, "prepare login cancel");
  ClearDetachedGrace(kPrepared.entity_id);
  UnbindClient(kPrepared.entity_id);
  (void)entity_mgr_.ClearSessionKey(kPrepared.entity_id);
  (void)entity_mgr_.AssignDbid(kPrepared.entity_id, kInvalidDBID);
  ent->MarkForDestroy();
  ReleaseCheckout(kPrepared.dbid, kPrepared.type_id);
  return true;
}

void BaseApp::ClearPreparedLoginEntity(EntityID entity_id) {
  auto it = prepared_login_requests_by_entity_.find(entity_id);
  if (it == prepared_login_requests_by_entity_.end()) {
    return;
  }

  prepared_login_entities_.erase(it->second);
  prepared_login_requests_by_entity_.erase(it);
}

void BaseApp::CancelPrepareLogin(uint32_t login_request_id, DatabaseID dbid) {
  for (auto it = pending_logins_.begin(); it != pending_logins_.end(); ++it) {
    if (it->second.login_request_id != login_request_id) {
      continue;
    }

    CancelInflightCheckout(it->first, it->second);
    FinishLoginFlow(it->second.dbid);
    pending_logins_.erase(it);
    return;
  }

  for (auto it = pending_force_logoffs_.begin(); it != pending_force_logoffs_.end(); ++it) {
    if (it->second.login_request_id != login_request_id) {
      continue;
    }

    FinishLoginFlow(it->second.dbid);
    pending_force_logoffs_.erase(it);
    return;
  }

  for (auto it = queued_logins_.begin(); it != queued_logins_.end(); ++it) {
    if (it->second.login_request_id != login_request_id) {
      continue;
    }

    queued_logins_.erase(it);
    return;
  }

  for (auto deferred_it = deferred_login_checkouts_.begin();
       deferred_it != deferred_login_checkouts_.end();) {
    auto& deferred = deferred_it->second;
    bool removed = false;
    for (auto entry_it = deferred.begin(); entry_it != deferred.end();) {
      if (entry_it->pending.login_request_id != login_request_id) {
        ++entry_it;
        continue;
      }

      ReleaseCheckout(entry_it->dbid, entry_it->type_id);
      FinishLoginFlow(entry_it->pending.dbid);
      entry_it = deferred.erase(entry_it);
      removed = true;
    }

    if (deferred.empty()) {
      deferred_it = deferred_login_checkouts_.erase(deferred_it);
    } else {
      ++deferred_it;
    }

    if (removed) {
      return;
    }
  }

  if (RollbackPreparedLoginEntity(login_request_id)) {
    return;
  }

  ATLAS_LOG_DEBUG("BaseApp: cancel prepare login request_id={} dbid={} ignored (not pending)",
                  login_request_id, dbid);
}

auto BaseApp::RetryLoginAfterCheckoutConflict(PendingLogin pending, DatabaseID dbid,
                                              const Address& holder_addr) -> bool {
  if (holder_addr.Port() == 0) {
    return false;
  }

  const uint32_t kForceRequestId = next_prepare_request_id_++;
  pending.force_logoff_holder_addr = holder_addr;
  pending.next_force_logoff_retry_at = {};
  pending.force_logoff_retry_count = 0;
  pending.waiting_for_remote_force_logoff_ack = false;
  pending_force_logoffs_[kForceRequestId] = std::move(pending);

  baseapp::ForceLogoff force_logoff;
  force_logoff.dbid = dbid;
  force_logoff.request_id = kForceRequestId;

  if (holder_addr == Network().RudpAddress()) {
    ProcessForceLogoffRequest(force_logoff);
    return true;
  }

  auto pending_it = pending_force_logoffs_.find(kForceRequestId);
  if (pending_it == pending_force_logoffs_.end()) {
    return true;
  }

  pending_it->second.waiting_for_remote_force_logoff_ack = true;
  ScheduleForceLogoffRetry(pending_it->second, Clock::now());

  if (auto* holder_ch = ResolveInternalChannel(holder_addr)) {
    const auto kSendResult = holder_ch->SendMessage(force_logoff);
    if (!kSendResult) {
      ATLAS_LOG_WARNING(
          "BaseApp: failed to send ForceLogoff dbid={} request_id={} to holder {}:{}: {}", dbid,
          kForceRequestId, holder_addr.Ip(), holder_addr.Port(), kSendResult.Error().Message());
    }
  }

  return true;
}

auto BaseApp::RestoreManagedEntity(EntityID entity_id, uint16_t type_id, DatabaseID dbid,
                                   std::span<const std::byte> blob) -> bool {
  if (!HasManagedEntityType(type_id)) {
    return true;
  }

  if (!native_provider_ || !native_provider_->restore_entity_fn()) {
    return true;
  }

  ClearNativeApiError();
  native_provider_->restore_entity_fn()(entity_id, type_id, dbid,
                                        reinterpret_cast<const uint8_t*>(blob.data()),
                                        static_cast<int32_t>(blob.size()));
  if (auto error = ConsumeNativeApiError()) {
    ATLAS_LOG_ERROR("BaseApp: restore_entity failed for entity={} type={} dbid={}: {}", entity_id,
                    type_id, dbid, *error);
    return false;
  }

  return true;
}

auto BaseApp::NotifyManagedEntityDestroyed(EntityID entity_id, std::string_view context) -> bool {
  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent || !HasManagedEntityType(ent->TypeId())) {
    return true;
  }

  if (!native_provider_ || !native_provider_->entity_destroyed_fn()) {
    return true;
  }

  ClearNativeApiError();
  native_provider_->entity_destroyed_fn()(entity_id);
  if (auto error = ConsumeNativeApiError()) {
    ATLAS_LOG_ERROR("BaseApp: entity_destroyed callback failed for entity={} during {}: {}",
                    entity_id, context, *error);
    return false;
  }

  return true;
}

auto BaseApp::CaptureLoadSnapshot() const -> LoadSnapshot {
  LoadSnapshot snapshot;
  snapshot.entity_count = static_cast<uint32_t>(entity_mgr_.Size());
  snapshot.proxy_count = static_cast<uint32_t>(entity_mgr_.ProxyCount());
  snapshot.pending_prepare_count = static_cast<uint32_t>(pending_logins_.size());
  snapshot.pending_force_logoff_count = static_cast<uint32_t>(pending_force_logoffs_.size());
  snapshot.detached_proxy_count = static_cast<uint32_t>(detached_proxies_.size());
  snapshot.logoff_in_flight_count = static_cast<uint32_t>(logoff_entities_in_flight_.size());
  snapshot.deferred_login_count = static_cast<uint32_t>(DeferredLoginCheckoutCount());
  return snapshot;
}

void BaseApp::UpdateLoadEstimate() {
  load_tracker_.ObserveTickComplete(Config().update_hertz, CaptureLoadSnapshot());
}

void BaseApp::ReportLoadToBaseAppMgr() {
  if (!baseappmgr_channel_ || app_id_ == 0) {
    return;
  }

  const auto kMsg = load_tracker_.BuildReport(app_id_, CaptureLoadSnapshot());
  (void)baseappmgr_channel_->SendMessage(kMsg);
}

void BaseApp::MaybeRequestMoreIds() {
  if (!dbapp_channel_ || !entity_mgr_.IsRangeLow()) return;

  auto count = id_client_.IdsToRequest();
  if (count == 0) return;

  dbapp::GetEntityIds req;
  req.count = count;
  (void)dbapp_channel_->SendMessage(req);
  ATLAS_LOG_INFO("BaseApp: requested {} EntityIDs from DBApp", count);
}

void BaseApp::OnPrepareLogin(Channel& ch, const login::PrepareLogin& msg) {
  ATLAS_LOG_DEBUG("BaseApp: prepare login request_id={} dbid={} type_id={} blob={}B from {}:{}",
                  msg.request_id, msg.dbid, msg.type_id, msg.entity_blob.size(),
                  ch.RemoteAddress().Ip(), ch.RemoteAddress().Port());
  PendingLogin pending;
  pending.login_request_id = msg.request_id;
  pending.loginapp_addr = ch.RemoteAddress();
  pending.type_id = msg.type_id;
  pending.dbid = msg.dbid;
  pending.session_key = msg.session_key;
  pending.created_at = Clock::now();
  pending.blob_prefetched = msg.blob_prefetched;
  pending.entity_blob = msg.entity_blob;
  SubmitPrepareLogin(std::move(pending));
  (void)msg.client_addr;
}

void BaseApp::OnCancelPrepareLogin(Channel& /*ch*/, const login::CancelPrepareLogin& msg) {
  CancelPrepareLogin(msg.request_id, msg.dbid);
}

void BaseApp::OnClientEventSeqReport(const baseapp::ClientEventSeqReport& msg) {
  if (msg.gap_delta == 0) return;
  client_event_seq_gaps_total_ += msg.gap_delta;
  ATLAS_LOG_WARNING("Client reported reliable-delta gap: entity={} delta={} total={}",
                    msg.base_entity_id, msg.gap_delta, client_event_seq_gaps_total_);
}

void BaseApp::OnForceLogoff(Channel& ch, const baseapp::ForceLogoff& msg) {
  if (pending_force_logoffs_.contains(msg.request_id)) {
    ProcessForceLogoffRequest(msg);
    return;
  }

  EntityID found_id = kInvalidEntityID;
  if (auto* ent = entity_mgr_.FindByDbid(msg.dbid)) {
    found_id = ent->EntityId();
  }

  if (found_id == kInvalidEntityID) {
    baseapp::ForceLogoffAck ack;
    ack.request_id = msg.request_id;
    ack.success = true;
    (void)ch.SendMessage(ack);
    return;
  }

  pending_remote_force_logoff_acks_[found_id].push_back({ch.RemoteAddress(), msg.request_id});

  auto* ent = entity_mgr_.Find(found_id);
  if (!ent || ent->IsPendingDestroy()) {
    FlushRemoteForceLogoffAcks(found_id, true);
    return;
  }

  if (ent->Dbid() == kInvalidDBID) {
    (void)FinalizeForceLogoff(found_id);
    FlushRemoteForceLogoffAcks(found_id, true);
    return;
  }

  ++force_logoff_total_;
  BeginLogoffPersist(found_id, ent->Dbid(), ent->TypeId(), 0);
}

void BaseApp::OnForceLogoffAck(Channel& /*ch*/, const baseapp::ForceLogoffAck& msg) {
  auto it = pending_force_logoffs_.find(msg.request_id);
  if (it == pending_force_logoffs_.end()) return;

  if (!msg.success) {
    PendingLogin& pending = it->second;
    login::PrepareLoginResult reply;
    reply.request_id = pending.login_request_id;
    reply.success = false;
    reply.error = "force_logoff_failed";
    SendPrepareLoginResult(pending.loginapp_addr, reply);
    FinishLoginFlow(pending.dbid);
    pending_force_logoffs_.erase(it);
    return;
  }

  // Logoff succeeded — now checkout entity from DBApp
  it->second.waiting_for_remote_force_logoff_ack = false;
  it->second.next_force_logoff_retry_at = {};
  ContinueLoginAfterForceLogoff(msg.request_id);
}

// ============================================================================
// on_client_authenticate — handle first message from a client
// ============================================================================

void BaseApp::OnClientAuthenticate(Channel& ch, const baseapp::Authenticate& msg) {
  auto* proxy = entity_mgr_.FindProxyBySession(msg.session_key);
  if (!proxy) {
    ++auth_fail_total_;
    ATLAS_LOG_WARNING("BaseApp: Authenticate: no matching session key");
    baseapp::AuthenticateResult res;
    res.success = false;
    res.error = "invalid_session";
    (void)ch.SendMessage(res);
    return;
  }

  if (!BindClient(proxy->EntityId(), ch.RemoteAddress())) {
    ++auth_fail_total_;
    baseapp::AuthenticateResult res;
    res.success = false;
    res.error = "bind_client_failed";
    (void)ch.SendMessage(res);
    return;
  }

  baseapp::AuthenticateResult res;
  res.success = true;
  res.entity_id = proxy->EntityId();
  res.type_id = proxy->TypeId();
  (void)ch.SendMessage(res);
  ++auth_success_total_;
  ClearPreparedLoginEntity(proxy->EntityId());

  ATLAS_LOG_DEBUG("BaseApp: client authenticated as entity={}", proxy->EntityId());
}

// ============================================================================
// on_client_base_rpc — handle exposed base method call from client
// ============================================================================

void BaseApp::OnClientBaseRpc(Channel& ch, const baseapp::ClientBaseRpc& msg) {
  // 1. Find the proxy bound to this client channel
  auto it = client_entity_index_.find(ch.RemoteAddress());
  if (it == client_entity_index_.end()) {
    ATLAS_LOG_WARNING("BaseApp: ClientBaseRpc from unauthenticated channel");
    return;
  }
  auto entity_id = it->second;

  // 2. Validate the RPC is exposed.
  auto& registry = EntityDefRegistry::Instance();
  auto* rpc_desc = registry.FindRpc(msg.rpc_id);
  if (!rpc_desc || rpc_desc->exposed == ExposedScope::kNone) {
    ATLAS_LOG_WARNING("BaseApp: client tried to call non-exposed base method (rpc_id=0x{:06X})",
                      msg.rpc_id);
    return;
  }

  // 2.5. Direction check. A client that tries to invoke a cell_methods
  // (direction 0x02) or client_methods (0x00) through the Base channel
  // is probing — either exploitation, or a generator bug. Either way we
  // reject loud and fast.
  if (rpc_desc->Direction() != 0x03) {
    ATLAS_LOG_WARNING(
        "BaseApp: ClientBaseRpc rpc_id=0x{:06X} has direction {}, expected 0x03 (Base)", msg.rpc_id,
        rpc_desc->Direction());
    return;
  }

  // 3. Dispatch to C# via the native callback.
  auto dispatch_fn = GetNativeProvider().dispatch_rpc_fn();
  if (!dispatch_fn) {
    ATLAS_LOG_WARNING("BaseApp: ClientBaseRpc: dispatch_rpc callback not registered");
    return;
  }
  dispatch_fn(entity_id, msg.rpc_id, reinterpret_cast<const uint8_t*>(msg.payload.data()),
              static_cast<int32_t>(msg.payload.size()));
}

// OnClientCellRpc — validate then forward to CellApp. Defence-in-depth: L1
// authenticated channel, L1.5 direction bits, L2 cross-entity scope here;
// CellApp::OnClientCellRpcForward re-checks on the other side.
void BaseApp::OnClientCellRpc(Channel& ch, const baseapp::ClientCellRpc& msg) {
  // L1: authenticated proxy binding. An un-authenticated channel has no
  // right to an entity id, so there's no way to stamp source_entity_id.
  auto it = client_entity_index_.find(ch.RemoteAddress());
  if (it == client_entity_index_.end()) {
    ATLAS_LOG_WARNING("BaseApp: ClientCellRpc from unauthenticated channel (rpc_id=0x{:06X})",
                      msg.rpc_id);
    return;
  }
  const auto source_entity_id = it->second;

  auto& registry = EntityDefRegistry::Instance();
  const auto* rpc_desc = registry.FindRpc(msg.rpc_id);
  if (rpc_desc == nullptr) {
    ATLAS_LOG_WARNING("BaseApp: ClientCellRpc unknown rpc_id=0x{:06X} (source entity={})",
                      msg.rpc_id, source_entity_id);
    return;
  }

  // L1.5: direction check. Cell RPCs must sit in the 0x02 space.
  if (rpc_desc->Direction() != 0x02) {
    ATLAS_LOG_WARNING(
        "BaseApp: ClientCellRpc rpc_id=0x{:06X} has direction {}, expected 0x02 (Cell)", msg.rpc_id,
        rpc_desc->Direction());
    return;
  }

  // Exposed check.
  if (rpc_desc->exposed == ExposedScope::kNone) {
    ATLAS_LOG_WARNING("BaseApp: client tried to call non-exposed cell method (rpc_id=0x{:06X})",
                      msg.rpc_id);
    return;
  }

  // L2: cross-entity check. OWN_CLIENT methods may only be invoked on
  // the caller's own entity; ALL_CLIENTS methods may target any entity
  // the caller sees in AoI (we don't re-validate AoI here — the cell
  // layer has that data and will drop misaddressed packets anyway).
  if (msg.target_entity_id != source_entity_id && rpc_desc->exposed != ExposedScope::kAllClients) {
    ATLAS_LOG_WARNING(
        "BaseApp: cross-entity ClientCellRpc blocked (source={} target={} rpc_id=0x{:06X} "
        "exposed={})",
        source_entity_id, msg.target_entity_id, msg.rpc_id,
        static_cast<uint8_t>(rpc_desc->exposed));
    return;
  }

  // Forward to the CellApp. `source_entity_id` is stamped HERE from the
  // authenticated proxy binding — the client cannot forge it. CellApp
  // re-checks everything (defence in depth) on the other side.
  //
  // ResolveCellChannelForEntity handles the multi-CellApp lookup
  // (entity → cell_addr → channel map). Stale routing (Offload in flight)
  // shows up either as an unknown peer (nullptr, drop with a warning) or
  // as the old CellApp rejecting the RPC because the entity is now a
  // Ghost there — CellApp's OnClientCellRpcForward soft guard catches it.
  auto* ch_out = ResolveCellChannelForEntity(msg.target_entity_id);
  if (ch_out == nullptr) {
    // Rate-limit: a partitioned or slow CellApp can flood this log, each
    // line a format+flush that further stretches the tick loop. One line
    // per second is enough to diagnose a late-binding or partition bug.
    // Runs on the single dispatcher thread, so static locals are safe.
    using SteadyClock = std::chrono::steady_clock;
    static SteadyClock::time_point last_log{};
    static uint64_t suppressed = 0;
    const auto now = SteadyClock::now();
    if (now - last_log >= std::chrono::seconds(1)) {
      ATLAS_LOG_WARNING(
          "BaseApp: ClientCellRpc dropped — no cell channel for target entity {} "
          "(rpc_id=0x{:06X}){}",
          msg.target_entity_id, msg.rpc_id,
          suppressed > 0 ? std::format(" [+{} similar in last 1s]", suppressed) : std::string{});
      last_log = now;
      suppressed = 0;
    } else {
      ++suppressed;
    }
    return;
  }

  cellapp::ClientCellRpcForward fwd;
  fwd.target_entity_id = msg.target_entity_id;
  fwd.source_entity_id = source_entity_id;
  fwd.rpc_id = msg.rpc_id;
  fwd.payload = msg.payload;  // copy; Serialize/Deserialize owns bytes
  (void)ch_out->SendMessage(fwd);
}

auto ResolveCellChannelByAddr(const std::unordered_map<Address, Channel*>& cellapp_channels,
                              const Address& cell_addr) -> Channel* {
  // Cell address 0:0 is the "not yet placed on any Cell" sentinel.
  if (cell_addr.Port() == 0) return nullptr;
  auto it = cellapp_channels.find(cell_addr);
  return it == cellapp_channels.end() ? nullptr : it->second;
}

auto BaseApp::ResolveCellChannelForEntity(EntityID target_entity_id) const -> Channel* {
  auto* target = entity_mgr_.Find(target_entity_id);
  if (target == nullptr) return nullptr;
  return ResolveCellChannelByAddr(cellapp_peers_.Channels(), target->CellAddr());
}

// ============================================================================
// Space creation via CellAppMgr
// ============================================================================

auto BaseApp::RequestCreateSpace(SpaceID space_id, SpaceCreatedCallback callback) -> uint32_t {
  if (cellappmgr_channel_ == nullptr) {
    ATLAS_LOG_WARNING("BaseApp: RequestCreateSpace({}) — no CellAppMgr channel yet", space_id);
    if (callback) callback(/*success=*/false, space_id, Address{});
    return 0;
  }
  if (space_id == kInvalidSpaceID) {
    ATLAS_LOG_WARNING("BaseApp: RequestCreateSpace rejected: space_id=0");
    if (callback) callback(/*success=*/false, space_id, Address{});
    return 0;
  }
  const uint32_t request_id = next_space_request_id_++;
  pending_space_creates_[request_id] = std::move(callback);

  cellappmgr::CreateSpaceRequest msg;
  msg.space_id = space_id;
  msg.request_id = request_id;
  msg.reply_addr = Network().RudpAddress();
  (void)cellappmgr_channel_->SendMessage(msg);
  return request_id;
}

void BaseApp::OnSpaceCreatedResult(Channel& /*ch*/, const cellappmgr::SpaceCreatedResult& msg) {
  auto it = pending_space_creates_.find(msg.request_id);
  if (it == pending_space_creates_.end()) {
    ATLAS_LOG_WARNING(
        "BaseApp: SpaceCreatedResult for unknown request_id={} (space_id={}, success={})",
        msg.request_id, msg.space_id, msg.success);
    return;
  }
  auto cb = std::move(it->second);
  pending_space_creates_.erase(it);
  if (cb) cb(msg.success, msg.space_id, msg.host_addr);
}

}  // namespace atlas
