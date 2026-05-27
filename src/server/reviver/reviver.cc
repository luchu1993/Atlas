#include "reviver.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "cellappmgr/cellappmgr_messages.h"
#include "foundation/log.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "server/machined_client.h"
#include "server/watcher.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace atlas {

namespace {

auto CurrentPid() -> uint32_t {
#if defined(_WIN32)
  return static_cast<uint32_t>(::_getpid());
#else
  return static_cast<uint32_t>(::getpid());
#endif
}

auto SanitizeLockSegment(std::string value) -> std::string {
  for (char& ch : value) {
    const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    if (!keep) ch = '_';
  }
  if (value.empty()) return "cellappmgr";
  return value;
}

auto IsLoopbackAddress(const Address& addr) -> bool {
  const uint32_t ip = addr.Ip();
  const auto* bytes = reinterpret_cast<const uint8_t*>(&ip);
  return bytes[0] == 127;
}

auto AgeMsSince(TimePoint t) -> int64_t {
  if (t.time_since_epoch() == Duration::zero()) return -1;
  return std::max<int64_t>(
      0, std::chrono::duration_cast<Milliseconds>(Clock::now() - t).count());
}

}  // namespace

Reviver::Reviver(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network) {}

auto Reviver::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher("reviver");
  NetworkInterface network(dispatcher);
  Reviver app(dispatcher, network);
  return app.RunApp(argc, argv);
}

auto Reviver::Init(int argc, char* argv[]) -> bool {
  if (argc > 0 && argv[0] != nullptr) self_exe_ = std::filesystem::absolute(argv[0]);
  if (!ManagerApp::Init(argc, argv)) return false;

  leader_lock_path_ = ResolveLeaderLockPath();
  startup_check_at_ = Clock::now() + std::chrono::milliseconds(500);
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kCellAppMgr,
      [this](const machined::BirthNotification& msg) { OnCellAppMgrBirth(msg); },
      [this](const machined::DeathNotification& msg) { OnCellAppMgrDeath(msg); });
  (void)Network().InterfaceTable().RegisterTypedHandler<cellappmgr::HealthProbeAck>(
      [this](const Address& src, Channel* ch, const cellappmgr::HealthProbeAck& msg) {
        OnCellAppMgrHeartbeatAck(src, ch, msg);
      });
  return true;
}

void Reviver::Fini() {
  if (restart_timer_.IsValid()) {
    (void)Dispatcher().CancelTimer(restart_timer_);
    restart_timer_ = {};
  }
  leader_lock_.reset();
  ManagerApp::Fini();
}

void Reviver::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<std::string>("reviver/cellappmgr/status",
                      std::function<std::string()>([this] { return CellAppMgrStatus(); }));
  wr.Add<bool>("reviver/cellappmgr/active",
               std::function<bool()>([this] { return cellappmgr_active_; }));
  wr.Add<uint32_t>("reviver/cellappmgr/active_pid",
                   std::function<uint32_t()>([this] { return last_cellappmgr_pid_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/active_generation",
                   std::function<uint64_t()>([this] {
                     return cellappmgr_active_generation_;
                   }));
  wr.Add<uint32_t>("reviver/cellappmgr/launched_pid",
                   std::function<uint32_t()>([this] { return launched_cellappmgr_pid_; }));
  wr.Add<std::string>("reviver/cellappmgr/output_path",
                      std::function<std::string()>([this] {
                        return Config().revive_cellappmgr_output_path.string();
                      }));
  wr.Add<bool>("reviver/cellappmgr/launch_pending",
               std::function<bool()>([this] { return launch_pending_; }));
  wr.Add<uint32_t>("reviver/cellappmgr/restart_attempts",
                   std::function<uint32_t()>([this] { return restart_attempts_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/launch_count",
                   std::function<uint64_t()>([this] { return launch_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/launch_failures",
                   std::function<uint64_t()>([this] { return launch_failures_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/launch_timeouts",
                   std::function<uint64_t()>([this] { return launch_timeout_count_; }));
  wr.Add<bool>("reviver/cellappmgr/restart_limit_reached",
               std::function<bool()>([this] { return restart_limit_reached_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/restart_limit_hits",
                   std::function<uint64_t()>([this] { return restart_limit_hit_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/liveness_failures",
                   std::function<uint64_t()>([this] { return liveness_failures_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/health_checks",
                   std::function<uint64_t()>([this] { return health_check_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/health_failures",
                   std::function<uint64_t()>([this] { return health_failure_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/manager_health_failures",
                   std::function<uint64_t()>([this] {
                     return manager_health_failure_count_;
                   }));
  wr.Add<uint64_t>("reviver/cellappmgr/manager_health_timeouts",
                   std::function<uint64_t()>([this] {
                     return manager_health_timeout_count_;
                   }));
  wr.Add<uint64_t>("reviver/cellappmgr/heartbeat_sent",
                   std::function<uint64_t()>([this] { return heartbeat_sent_count_; }));
  wr.Add<int>("reviver/cellappmgr/heartbeat_timeout_ms",
              std::function<int()>([this] {
                return Config().revive_cellappmgr_heartbeat_timeout_ms;
              }));
  wr.Add<uint64_t>("reviver/cellappmgr/heartbeat_acks",
                   std::function<uint64_t()>([this] { return heartbeat_ack_count_; }));
  wr.Add<int64_t>("reviver/cellappmgr/heartbeat_last_ack_age_ms",
                  std::function<int64_t()>(
                      [this] { return HeartbeatLastAckAgeMsForWatcher(); }));
  wr.Add<uint64_t>("reviver/cellappmgr/heartbeat_failures",
                   std::function<uint64_t()>([this] { return heartbeat_failure_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/heartbeat_timeouts",
                   std::function<uint64_t()>([this] { return heartbeat_timeout_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/heartbeat_last_game_time",
                   std::function<uint64_t()>([this] { return heartbeat_last_game_time_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/heartbeat_snapshot_saves",
                   std::function<uint64_t()>([this] { return heartbeat_snapshot_saves_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/heartbeat_snapshot_failures",
                   std::function<uint64_t()>([this] {
                     return heartbeat_snapshot_failures_;
                   }));
  wr.Add<bool>("reviver/cellappmgr/heartbeat_snapshot_dirty",
               std::function<bool()>([this] { return heartbeat_snapshot_dirty_; }));
  wr.Add<bool>("reviver/cellappmgr/heartbeat_snapshot_save_stale",
               std::function<bool()>([this] { return heartbeat_snapshot_save_stale_; }));
  wr.Add<std::string>("reviver/cellappmgr/heartbeat_snapshot_status",
                      std::function<std::string()>(
                          [this] { return CellAppMgrHeartbeatSnapshotStatus(); }));
  wr.Add<uint64_t>("reviver/cellappmgr/forced_terminations",
                   std::function<uint64_t()>([this] { return forced_termination_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/registry_audits",
                   std::function<uint64_t()>([this] { return registry_audit_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/registry_missing",
                   std::function<uint64_t()>([this] { return registry_missing_count_; }));
  wr.Add<std::string>("reviver/cellappmgr/last_error",
                      std::function<std::string()>([this] { return last_error_; }));
  wr.Add<bool>("reviver/leader/active",
               std::function<bool()>([this] { return HasLeadership(); }));
  wr.Add<std::string>("reviver/leader/lock_path",
                      std::function<std::string()>(
                          [this] { return leader_lock_path_.string(); }));
  wr.Add<uint64_t>("reviver/leader/acquire_count",
                   std::function<uint64_t()>([this] { return leader_lock_acquires_; }));
  wr.Add<uint64_t>("reviver/leader/acquire_failures",
                   std::function<uint64_t()>([this] { return leader_lock_failures_; }));
}

void Reviver::OnTickComplete() {
  AuditLeadership();
  if (!HasLeadership()) return;
  AuditColdStart();
  AuditCellAppMgrLaunch();
  AuditCellAppMgrLiveness();
  AuditCellAppMgrHeartbeat();
  AuditCellAppMgrHealth();
  AuditCellAppMgrRegistry();
}

void Reviver::OnCellAppMgrBirth(const machined::BirthNotification& msg) {
  if (!MatchesTargetName(msg.name)) return;
  if (restart_timer_.IsValid()) {
    (void)Dispatcher().CancelTimer(restart_timer_);
    restart_timer_ = {};
  }
  RememberCellAppMgr(msg.name, msg.internal_addr, msg.pid);
}

void Reviver::RememberCellAppMgr(std::string_view name, const Address& addr, uint32_t pid) {
  const bool target_changed =
      !cellappmgr_active_ || last_cellappmgr_addr_ != addr || last_cellappmgr_pid_ != pid;
  if (!target_changed) return;
  if (last_cellappmgr_addr_.Port() != 0) {
    Network().DisconnectChannel(last_cellappmgr_addr_);
  }
  if (addr != last_cellappmgr_addr_ && addr.Port() != 0) {
    Network().DisconnectChannel(addr);
  }
  ++cellappmgr_active_generation_;
  cellappmgr_active_ = true;
  last_cellappmgr_addr_ = addr;
  last_cellappmgr_pid_ = pid;
  ResetCellAppMgrLaunch();
  ResetCellAppMgrManagerHealth();
  ResetCellAppMgrHeartbeat();
  heartbeat_failure_streak_ = 0;
  manager_health_failure_streak_ = 0;
  registry_missing_streak_ = 0;
  heartbeat_last_game_time_ = 0;
  heartbeat_snapshot_saves_ = 0;
  heartbeat_snapshot_failures_ = 0;
  heartbeat_snapshot_dirty_ = false;
  heartbeat_snapshot_save_stale_ = false;
  last_error_.clear();
  ATLAS_LOG_INFO("Reviver: CellAppMgr active name={} pid={} addr={}",
                 name, pid, addr.ToString());
}

void Reviver::OnCellAppMgrDeath(const machined::DeathNotification& msg) {
  if (!MatchesTargetName(msg.name)) return;
  cellappmgr_active_ = false;
  ResetCellAppMgrManagerHealth();
  ResetCellAppMgrHeartbeat();
  last_cellappmgr_addr_ = msg.internal_addr;
  if (msg.reason == 0) {
    ATLAS_LOG_INFO("Reviver: CellAppMgr death name={} reason={} addr={}",
                   msg.name, msg.reason, msg.internal_addr.ToString());
  } else {
    ATLAS_LOG_WARNING("Reviver: CellAppMgr death name={} reason={} addr={}",
                      msg.name, msg.reason, msg.internal_addr.ToString());
  }
  if (!HasLeadership()) return;
  if (msg.reason == 0) return;
  ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::AuditLeadership() {
  if (HasLeadership()) return;
  const auto now = Clock::now();
  if (next_leader_lock_attempt_ != TimePoint{} && now < next_leader_lock_attempt_) return;
  next_leader_lock_attempt_ = now + std::chrono::seconds(1);

  auto lock = fs::ScopedFileLock::TryAcquire(leader_lock_path_, LeaderLockContent());
  if (!lock) {
    ++leader_lock_failures_;
    if (lock.Error().Code() != ErrorCode::kAlreadyExists) {
      last_error_ = std::format("leader lock failed: {}", lock.Error().Message());
      ATLAS_LOG_WARNING("Reviver: {}", last_error_);
    }
    return;
  }

  leader_lock_.emplace(std::move(*lock));
  ++leader_lock_acquires_;
  startup_checked_ = false;
  startup_check_at_ = Clock::now();
  last_error_.clear();
  ATLAS_LOG_INFO("Reviver: acquired leader lock {}", leader_lock_path_.string());
}

void Reviver::AuditColdStart() {
  if (startup_checked_ || Clock::now() < startup_check_at_) return;
  if (!Config().revive_cellappmgr_on_start || cellappmgr_active_) {
    startup_checked_ = true;
    return;
  }
  if (cellappmgr_query_pending_) return;
  startup_checked_ = true;
  cellappmgr_query_pending_ = true;
  GetMachinedClient().QueryAsync(ProcessType::kCellAppMgr,
                                 [this](std::vector<machined::ProcessInfo> infos) {
    cellappmgr_query_pending_ = false;
    if (cellappmgr_active_) return;
    for (const auto& info : infos) {
      if (!MatchesTargetName(info.name)) continue;
      RememberCellAppMgr(info.name, info.internal_addr, info.pid);
      return;
    }
    ScheduleCellAppMgrRestart(Duration::zero());
  });
}

void Reviver::AuditCellAppMgrRegistry() {
  if (!cellappmgr_active_ || cellappmgr_query_pending_) return;
  if (!GetMachinedClient().IsConnected()) return;
  const int interval_ms = Config().revive_cellappmgr_audit_interval_ms;
  if (interval_ms <= 0) return;
  const auto now = Clock::now();
  if (next_registry_audit_at_ != TimePoint{} && now < next_registry_audit_at_) return;
  next_registry_audit_at_ = now + Milliseconds(interval_ms);
  cellappmgr_query_pending_ = true;
  ++registry_audit_count_;
  GetMachinedClient().QueryAsync(ProcessType::kCellAppMgr,
                                 [this](std::vector<machined::ProcessInfo> infos) {
    OnCellAppMgrRegistryAudit(std::move(infos));
  });
}

void Reviver::AuditCellAppMgrLaunch() {
  if (!launch_pending_ || cellappmgr_active_) return;
  if (Clock::now() < launch_deadline_) return;

  launch_pending_ = false;
  launch_deadline_ = {};
  ++launch_timeout_count_;
  ++launch_failures_;
  last_error_ =
      std::format("CellAppMgr launch pid {} did not register", launched_cellappmgr_pid_);
  ATLAS_LOG_WARNING("Reviver: {}", last_error_);
  TerminateLaunchedCellAppMgr("launch registration timeout");
  ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::AuditCellAppMgrLiveness() {
  if (!cellappmgr_active_ || last_cellappmgr_pid_ == 0) return;
  if (!CanCheckLocalPid()) return;
  if (IsProcessAlive(last_cellappmgr_pid_)) return;

  cellappmgr_active_ = false;
  ResetCellAppMgrManagerHealth();
  ResetCellAppMgrHeartbeat();
  ++liveness_failures_;
  last_error_ = std::format("CellAppMgr pid {} is no longer alive", last_cellappmgr_pid_);
  ATLAS_LOG_WARNING("Reviver: {}", last_error_);
  ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::AuditCellAppMgrHeartbeat() {
  if (!cellappmgr_active_) return;
  const int interval_ms = Config().revive_cellappmgr_health_interval_ms;
  if (interval_ms <= 0) return;
  const auto now = Clock::now();
  const auto interval = Milliseconds(interval_ms);
  const int heartbeat_timeout_ms = Config().revive_cellappmgr_heartbeat_timeout_ms > 0
                                       ? Config().revive_cellappmgr_heartbeat_timeout_ms
                                       : std::max(500, interval_ms * 2);
  const auto timeout = Milliseconds(heartbeat_timeout_ms);

  if (heartbeat_pending_) {
    if (now - heartbeat_sent_at_ < timeout) return;
    heartbeat_pending_ = false;
    heartbeat_pending_nonce_ = 0;
    next_heartbeat_at_ = now + interval;
    ++heartbeat_timeout_count_;
    RecordCellAppMgrHeartbeatFailure("CellAppMgr heartbeat timed out");
    return;
  }

  if (next_heartbeat_at_ != TimePoint{} && now < next_heartbeat_at_) return;
  next_heartbeat_at_ = now + interval;

  // ConnectRudpNocwnd is idempotent for live addrs — returns the cached
  // ReliableUdpChannel; rebuilds only after a real disconnect/eviction.
  auto ch = Network().ConnectRudpNocwnd(last_cellappmgr_addr_);
  if (!ch || *ch == nullptr) {
    RecordCellAppMgrHeartbeatFailure("CellAppMgr heartbeat channel failed");
    return;
  }

  cellappmgr::HealthProbe msg;
  msg.nonce = ++heartbeat_nonce_;
  heartbeat_pending_nonce_ = msg.nonce;
  heartbeat_pending_ = true;
  heartbeat_sent_at_ = now;
  ++heartbeat_sent_count_;

  if (auto send = (*ch)->SendMessage(msg); !send) {
    heartbeat_pending_ = false;
    heartbeat_pending_nonce_ = 0;
    RecordCellAppMgrHeartbeatFailure("CellAppMgr heartbeat send failed");
  }
}

void Reviver::OnCellAppMgrHeartbeatAck(const Address& src, Channel*,
                                       const cellappmgr::HealthProbeAck& msg) {
  if (!cellappmgr_active_) return;
  if (src != last_cellappmgr_addr_) return;
  if (!heartbeat_pending_ || msg.nonce != heartbeat_pending_nonce_) return;
  heartbeat_pending_ = false;
  heartbeat_pending_nonce_ = 0;
  heartbeat_failure_streak_ = 0;
  restart_attempts_ = 0;
  restart_limit_reached_ = false;
  last_error_.clear();
  heartbeat_last_ack_at_ = Clock::now();
  heartbeat_last_game_time_ = msg.game_time;
  heartbeat_snapshot_saves_ = msg.snapshot_saves;
  heartbeat_snapshot_failures_ = msg.snapshot_failures;
  heartbeat_snapshot_dirty_ = msg.snapshot_dirty;
  heartbeat_snapshot_save_stale_ = msg.snapshot_save_stale;
  ++heartbeat_ack_count_;
}

void Reviver::AuditCellAppMgrHealth() {
  if (!cellappmgr_active_) return;
  const int interval_ms = Config().revive_cellappmgr_health_interval_ms;
  if (interval_ms <= 0) return;
  const auto now = Clock::now();
  if (cellappmgr_health_pending_) {
    const auto timeout_ms =
        std::max(1, Config().revive_cellappmgr_manager_health_timeout_ms);
    const auto timeout = Milliseconds(timeout_ms);
    if (health_check_sent_at_ != TimePoint{} && now - health_check_sent_at_ < timeout) {
      return;
    }
    cellappmgr_health_pending_ = false;
    health_check_sent_at_ = {};
    ++health_check_generation_;
    ++manager_health_timeout_count_;
    RecordCellAppMgrManagerHealthFailure("CellAppMgr manager health watcher timed out");
    return;
  }
  if (!GetMachinedClient().IsConnected()) return;
  if (next_health_check_at_ != TimePoint{} && now < next_health_check_at_) return;
  next_health_check_at_ = now + Milliseconds(interval_ms);
  cellappmgr_health_pending_ = true;
  health_check_sent_at_ = now;
  ++health_check_count_;
  const uint64_t generation = ++health_check_generation_;
  GetMachinedClient().QueryWatcher(
      ProcessType::kCellAppMgr, Config().revive_cellappmgr_name, "app/uptime_seconds",
      [this, generation](bool found, const std::string&, const std::string&) {
        OnCellAppMgrHealth(generation, found);
      });
}

void Reviver::OnCellAppMgrHealth(uint64_t generation, bool found) {
  if (generation != health_check_generation_) return;
  cellappmgr_health_pending_ = false;
  health_check_sent_at_ = {};
  if (!cellappmgr_active_) return;
  if (found) {
    manager_health_failure_streak_ = 0;
    return;
  }
  RecordCellAppMgrManagerHealthFailure("CellAppMgr manager health watcher did not respond");
}

void Reviver::OnCellAppMgrRegistryAudit(std::vector<machined::ProcessInfo> infos) {
  cellappmgr_query_pending_ = false;
  if (!cellappmgr_active_) return;
  for (const auto& info : infos) {
    if (!MatchesTargetName(info.name)) continue;
    if (last_cellappmgr_pid_ != info.pid || last_cellappmgr_addr_ != info.internal_addr) {
      RememberCellAppMgr(info.name, info.internal_addr, info.pid);
    } else {
      registry_missing_streak_ = 0;
    }
    return;
  }

  ++registry_missing_streak_;
  ++registry_missing_count_;
  const auto threshold =
      static_cast<uint32_t>(std::max(1, Config().revive_cellappmgr_missing_audit_threshold));
  if (registry_missing_streak_ < threshold) return;

  cellappmgr_active_ = false;
  ResetCellAppMgrManagerHealth();
  ResetCellAppMgrHeartbeat();
  TerminateCellAppMgrIfLocal("registry missing");
  last_error_ = std::format("CellAppMgr missing from machined registry after {} audit(s)",
                            registry_missing_streak_);
  ATLAS_LOG_WARNING("Reviver: {}", last_error_);
  ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::RecordCellAppMgrHeartbeatFailure(std::string_view reason) {
  ++heartbeat_failure_count_;
  if (last_cellappmgr_addr_.Port() != 0) {
    Network().DisconnectChannel(last_cellappmgr_addr_);
  }
  RecordCellAppMgrHealthFailure(reason, heartbeat_failure_streak_);
}

void Reviver::RecordCellAppMgrManagerHealthFailure(std::string_view reason) {
  ++manager_health_failure_count_;
  RecordCellAppMgrHealthFailure(reason, manager_health_failure_streak_);
}

void Reviver::RecordCellAppMgrHealthFailure(std::string_view reason, uint32_t& streak) {
  ++streak;
  ++health_failure_count_;
  const auto threshold =
      static_cast<uint32_t>(std::max(1, Config().revive_cellappmgr_health_failure_threshold));
  if (streak < threshold) return;

  cellappmgr_active_ = false;
  ResetCellAppMgrManagerHealth();
  ResetCellAppMgrHeartbeat();
  TerminateCellAppMgrIfLocal(reason);
  last_error_ = std::format("{} after {} check(s)", reason, streak);
  ATLAS_LOG_WARNING("Reviver: {}", last_error_);
  ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::ResetCellAppMgrHeartbeat() {
  heartbeat_pending_ = false;
  heartbeat_pending_nonce_ = 0;
  heartbeat_sent_at_ = {};
  next_heartbeat_at_ = {};
  heartbeat_last_ack_at_ = {};
}

void Reviver::ResetCellAppMgrManagerHealth() {
  cellappmgr_health_pending_ = false;
  health_check_sent_at_ = {};
  next_health_check_at_ = {};
  ++health_check_generation_;
}

void Reviver::ResetCellAppMgrLaunch() {
  launch_pending_ = false;
  launch_deadline_ = {};
}

void Reviver::TerminateCellAppMgrIfLocal(std::string_view reason) {
  if (last_cellappmgr_pid_ == 0 || !CanCheckLocalPid()) return;
  if (!IsProcessAlive(last_cellappmgr_pid_)) return;
  if (!TerminateProcessByPid(last_cellappmgr_pid_)) return;
  ++forced_termination_count_;
  ATLAS_LOG_WARNING("Reviver: terminated CellAppMgr pid={} reason={}",
                    last_cellappmgr_pid_, reason);
}

void Reviver::TerminateLaunchedCellAppMgr(std::string_view reason) {
  if (launched_cellappmgr_pid_ == 0) return;
  if (!IsProcessAlive(launched_cellappmgr_pid_)) return;
  if (!TerminateProcessByPid(launched_cellappmgr_pid_)) return;
  ++forced_termination_count_;
  ATLAS_LOG_WARNING("Reviver: terminated launched CellAppMgr pid={} reason={}",
                    launched_cellappmgr_pid_, reason);
}

void Reviver::ScheduleCellAppMgrRestart(Duration delay) {
  if (!HasLeadership()) return;
  if (restart_timer_.IsValid()) return;
  if (restart_attempts_ >= static_cast<uint32_t>(std::max(0, Config().revive_max_restarts))) {
    if (!restart_limit_reached_) ++restart_limit_hit_count_;
    restart_limit_reached_ = true;
    last_error_ = "restart limit reached";
    ATLAS_LOG_ERROR("Reviver: CellAppMgr restart limit reached");
    return;
  }
  const int cap_ms = Config().revive_restart_backoff_cap_ms;
  if (cap_ms > 0 && delay > Duration::zero()) {
    const auto base_ms = std::chrono::duration_cast<Milliseconds>(delay).count();
    if (base_ms > 0) {
      const auto shift = std::min<uint32_t>(restart_attempts_, 16);
      const int64_t scaled = base_ms << shift;
      const auto bounded = std::min<int64_t>(scaled, cap_ms);
      delay = std::chrono::duration_cast<Duration>(Milliseconds(bounded));
    }
  }
  restart_timer_ = Dispatcher().AddTimer(delay, [this](TimerHandle) {
    restart_timer_ = {};
    LaunchCellAppMgr();
  });
}

void Reviver::LaunchCellAppMgr() {
  if (!HasLeadership()) return;
  if (cellappmgr_active_) return;
  ++restart_attempts_;
  const auto exe = ResolveCellAppMgrExe();
  const uint16_t port = CellAppMgrPortForLaunch();
  if (exe.empty() || !std::filesystem::exists(exe)) {
    ++launch_failures_;
    last_error_ = std::format("CellAppMgr exe not found: {}", exe.string());
    ATLAS_LOG_ERROR("Reviver: {}", last_error_);
    ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
    return;
  }
  if (port == 0) {
    ++launch_failures_;
    last_error_ = "CellAppMgr internal port is not configured";
    ATLAS_LOG_ERROR("Reviver: {}", last_error_);
    ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
    return;
  }

  std::vector<std::string> args;
  if (!Config().config_path.empty()) {
    args.push_back("--config");
    args.push_back(Config().config_path.string());
  }
  args.insert(args.end(), {
      "--type", "cellappmgr",
      "--name", Config().revive_cellappmgr_name,
      "--internal-port", std::to_string(port),
      "--machined", Config().machined_address.ToString(),
      "--update-hertz", std::to_string(Config().revive_cellappmgr_update_hertz)});
  const auto snapshot_path = Config().revive_cellappmgr_snapshot_path.empty()
                                 ? Config().snapshot_path
                                 : Config().revive_cellappmgr_snapshot_path;
  if (!snapshot_path.empty()) {
    args.push_back("--snapshot-path");
    args.push_back(snapshot_path.string());
    const int snapshot_interval_ms = Config().revive_cellappmgr_snapshot_interval_ms >= 0
                                         ? Config().revive_cellappmgr_snapshot_interval_ms
                                         : Config().snapshot_interval_ms;
    args.push_back("--snapshot-interval-ms");
    args.push_back(std::to_string(std::max(0, snapshot_interval_ms)));
  }

  ProcessLaunchOptions opts;
  opts.exe = exe;
  opts.args = std::move(args);
  opts.working_directory = exe.parent_path();
  opts.output_path = Config().revive_cellappmgr_output_path;
  auto pid = LaunchDetachedProcess(std::move(opts));
  if (!pid) {
    ++launch_failures_;
    last_error_ = pid.Error().Message();
    ATLAS_LOG_ERROR("Reviver: failed to launch CellAppMgr: {}", last_error_);
    ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
    return;
  }

  launched_cellappmgr_pid_ = *pid;
  launch_pending_ = true;
  launch_deadline_ =
      Clock::now() + Milliseconds(std::max(1, Config().revive_cellappmgr_launch_timeout_ms));
  ++launch_count_;
  ATLAS_LOG_WARNING("Reviver: launched CellAppMgr attempt={} pid={} exe={} port={}",
                    restart_attempts_, launched_cellappmgr_pid_, exe.string(), port);
}

auto Reviver::HasLeadership() const -> bool {
  return leader_lock_.has_value() && leader_lock_->IsHeld();
}

auto Reviver::MatchesTargetName(std::string_view name) const -> bool {
  const auto& target = Config().revive_cellappmgr_name;
  return target.empty() || name == target;
}

auto Reviver::CanCheckLocalPid() const -> bool {
  return last_cellappmgr_pid_ == launched_cellappmgr_pid_ ||
         IsLoopbackAddress(last_cellappmgr_addr_);
}

auto Reviver::ResolveLeaderLockPath() const -> std::filesystem::path {
  if (!Config().revive_leader_lock_path.empty()) {
    return std::filesystem::absolute(Config().revive_leader_lock_path);
  }
  const auto target = SanitizeLockSegment(Config().revive_cellappmgr_name);
  const uint16_t port = Config().revive_cellappmgr_internal_port;
  auto base = Config().revive_cellappmgr_snapshot_path.parent_path();
  if (base.empty()) base = Config().snapshot_path.parent_path();
  if (base.empty()) base = fs::TempDirectory();
  return base / std::format("atlas_reviver_{}_{}.lock", target, port);
}

auto Reviver::ResolveCellAppMgrExe() const -> std::filesystem::path {
  if (!Config().revive_cellappmgr_exe.empty()) {
    return std::filesystem::absolute(Config().revive_cellappmgr_exe);
  }
#if defined(_WIN32)
  constexpr auto kExeName = "atlas_cellappmgr.exe";
#else
  constexpr auto kExeName = "atlas_cellappmgr";
#endif
  if (!self_exe_.empty()) return self_exe_.parent_path() / kExeName;
  return std::filesystem::path{kExeName};
}

auto Reviver::CellAppMgrPortForLaunch() const -> uint16_t {
  if (Config().revive_cellappmgr_internal_port != 0) {
    return Config().revive_cellappmgr_internal_port;
  }
  return last_cellappmgr_addr_.Port();
}

auto Reviver::LeaderLockContent() const -> std::string {
  return std::format("process={} pid={} target={} port={}\n", Config().process_name,
                     CurrentPid(), Config().revive_cellappmgr_name,
                     Config().revive_cellappmgr_internal_port);
}

auto Reviver::CellAppMgrStatus() const -> std::string {
  if (!HasLeadership()) return "standby";
  if (restart_limit_reached_) return "restart_limited";
  if (cellappmgr_active_) return "active";
  if (launch_pending_) return "launching";
  if (restart_timer_.IsValid()) return "restart_scheduled";
  if (cellappmgr_query_pending_) return "querying";
  return "idle";
}

auto Reviver::HeartbeatLastAckAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(heartbeat_last_ack_at_);
}

auto Reviver::CellAppMgrHeartbeatSnapshotStatus() const -> std::string {
  const char* state = "unknown";
  if (!cellappmgr_active_) {
    state = "inactive";
  } else if (heartbeat_last_ack_at_.time_since_epoch() == Duration::zero()) {
    state = "unknown";
  } else if (heartbeat_snapshot_failures_ > 0) {
    state = "failed";
  } else if (heartbeat_snapshot_dirty_) {
    state = "dirty";
  } else if (heartbeat_snapshot_save_stale_) {
    state = "stale";
  } else {
    state = "ready";
  }
  return std::format("state={} saves={} failures={} dirty={} stale={} game_time={} "
                     "ack_age_ms={}",
                     state, heartbeat_snapshot_saves_, heartbeat_snapshot_failures_,
                     heartbeat_snapshot_dirty_ ? 1 : 0,
                     heartbeat_snapshot_save_stale_ ? 1 : 0, heartbeat_last_game_time_,
                     HeartbeatLastAckAgeMsForWatcher());
}

}  // namespace atlas
