#include "reviver.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "baseappmgr/baseappmgr_messages.h"
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
  if (value.empty()) return "target";
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

void Reviver::InitTarget(ManagedTarget& t, ProcessType pt, std::string slug) {
  t.process_type = pt;
  t.slug = std::move(slug);
  if (pt == ProcessType::kCellAppMgr) {
    t.configured_name = Config().revive_cellappmgr_name;
    t.exe = Config().revive_cellappmgr_exe;
    t.internal_port = Config().revive_cellappmgr_internal_port;
    t.snapshot_path = Config().revive_cellappmgr_snapshot_path;
    t.output_path = Config().revive_cellappmgr_output_path;
    t.snapshot_interval_ms = Config().revive_cellappmgr_snapshot_interval_ms;
    t.update_hertz = Config().revive_cellappmgr_update_hertz;
    t.launch_timeout_ms = Config().revive_cellappmgr_launch_timeout_ms;
    t.health_interval_ms = Config().revive_cellappmgr_health_interval_ms;
    t.heartbeat_timeout_ms = Config().revive_cellappmgr_heartbeat_timeout_ms;
    t.manager_health_timeout_ms = Config().revive_cellappmgr_manager_health_timeout_ms;
    t.health_failure_threshold = Config().revive_cellappmgr_health_failure_threshold;
    t.audit_interval_ms = Config().revive_cellappmgr_audit_interval_ms;
    t.missing_audit_threshold = Config().revive_cellappmgr_missing_audit_threshold;
    t.on_start = Config().revive_cellappmgr_on_start;
    t.leader_lock_path = Config().revive_leader_lock_path;
  } else {
    t.configured_name = Config().revive_baseappmgr_name;
    t.exe = Config().revive_baseappmgr_exe;
    t.internal_port = Config().revive_baseappmgr_internal_port;
    t.snapshot_path = Config().revive_baseappmgr_snapshot_path;
    t.output_path = Config().revive_baseappmgr_output_path;
    t.snapshot_interval_ms = Config().revive_baseappmgr_snapshot_interval_ms;
    t.update_hertz = Config().revive_baseappmgr_update_hertz;
    t.launch_timeout_ms = Config().revive_baseappmgr_launch_timeout_ms;
    // BaseAppMgr reuses CellAppMgr's per-target timing knobs for now;
    // separate knobs can be added if BaseAppMgr ever needs different cadence.
    t.health_interval_ms = Config().revive_cellappmgr_health_interval_ms;
    t.heartbeat_timeout_ms = Config().revive_cellappmgr_heartbeat_timeout_ms;
    t.manager_health_timeout_ms = Config().revive_cellappmgr_manager_health_timeout_ms;
    t.health_failure_threshold = Config().revive_cellappmgr_health_failure_threshold;
    t.audit_interval_ms = Config().revive_cellappmgr_audit_interval_ms;
    t.missing_audit_threshold = Config().revive_cellappmgr_missing_audit_threshold;
    t.on_start = Config().revive_baseappmgr_on_start;
    t.leader_lock_path = Config().revive_baseappmgr_leader_lock_path;
  }
  if (TargetEnabled(t)) {
    t.leader_lock_path = ResolveLeaderLockPath(t);
  }
}

auto Reviver::TargetEnabled(const ManagedTarget& t) const -> bool {
  // Enabled when on_start is requested OR an explicit exe/port pair has
  // been configured. Empty everything → silently disabled (handy for tests
  // that only care about one target).
  if (t.on_start) return true;
  if (!t.exe.empty()) return true;
  if (t.internal_port != 0) return true;
  return false;
}

auto Reviver::Init(int argc, char* argv[]) -> bool {
  if (argc > 0 && argv[0] != nullptr) self_exe_ = std::filesystem::absolute(argv[0]);
  if (!ManagerApp::Init(argc, argv)) return false;

  InitTarget(cellappmgr_target_, ProcessType::kCellAppMgr, "cellappmgr");
  InitTarget(baseappmgr_target_, ProcessType::kBaseAppMgr, "baseappmgr");

  cellappmgr_target_.startup_check_at = Clock::now() + std::chrono::milliseconds(500);
  baseappmgr_target_.startup_check_at = Clock::now() + std::chrono::milliseconds(500);

  if (TargetEnabled(cellappmgr_target_)) {
    GetMachinedClient().Subscribe(
        machined::ListenerType::kBoth, ProcessType::kCellAppMgr,
        [this](const machined::BirthNotification& msg) {
          OnTargetBirth(cellappmgr_target_, msg);
        },
        [this](const machined::DeathNotification& msg) {
          OnTargetDeath(cellappmgr_target_, msg);
        });
    (void)Network().InterfaceTable().RegisterTypedHandler<cellappmgr::HealthProbeAck>(
        [this](const Address& src, Channel* ch, const cellappmgr::HealthProbeAck& msg) {
          OnCellAppMgrHeartbeatAck(src, ch, msg);
        });
  }
  if (TargetEnabled(baseappmgr_target_)) {
    GetMachinedClient().Subscribe(
        machined::ListenerType::kBoth, ProcessType::kBaseAppMgr,
        [this](const machined::BirthNotification& msg) {
          OnTargetBirth(baseappmgr_target_, msg);
        },
        [this](const machined::DeathNotification& msg) {
          OnTargetDeath(baseappmgr_target_, msg);
        });
    (void)Network().InterfaceTable().RegisterTypedHandler<baseappmgr::HealthProbeAck>(
        [this](const Address& src, Channel* ch, const baseappmgr::HealthProbeAck& msg) {
          OnBaseAppMgrHeartbeatAck(src, ch, msg);
        });
  }
  return true;
}

void Reviver::Fini() {
  for (ManagedTarget* tp : {&cellappmgr_target_, &baseappmgr_target_}) {
    if (tp->restart_timer.IsValid()) {
      (void)Dispatcher().CancelTimer(tp->restart_timer);
      tp->restart_timer = {};
    }
    tp->leader_lock.reset();
  }
  ManagerApp::Fini();
}

void Reviver::RegisterTargetWatchers(ManagedTarget& t) {
  auto& wr = GetWatcherRegistry();
  const auto root = std::format("reviver/{}", t.slug);
  wr.Add<std::string>(root + "/status",
                      std::function<std::string()>([this, &t] { return TargetStatus(t); }));
  wr.Add<bool>(root + "/active",
               std::function<bool()>([&t] { return t.active; }));
  wr.Add<uint32_t>(root + "/active_pid",
                   std::function<uint32_t()>([&t] { return t.last_pid; }));
  wr.Add<uint64_t>(root + "/active_generation",
                   std::function<uint64_t()>([&t] { return t.active_generation; }));
  wr.Add<uint32_t>(root + "/launched_pid",
                   std::function<uint32_t()>([&t] { return t.launched_pid; }));
  wr.Add<std::string>(root + "/output_path",
                      std::function<std::string()>([&t] { return t.output_path.string(); }));
  wr.Add<bool>(root + "/launch_pending",
               std::function<bool()>([&t] { return t.launch_pending; }));
  wr.Add<uint32_t>(root + "/restart_attempts",
                   std::function<uint32_t()>([&t] { return t.restart_attempts; }));
  wr.Add<uint64_t>(root + "/launch_count",
                   std::function<uint64_t()>([&t] { return t.launch_count; }));
  wr.Add<uint64_t>(root + "/launch_failures",
                   std::function<uint64_t()>([&t] { return t.launch_failures; }));
  wr.Add<uint64_t>(root + "/launch_timeouts",
                   std::function<uint64_t()>([&t] { return t.launch_timeout_count; }));
  wr.Add<bool>(root + "/restart_limit_reached",
               std::function<bool()>([&t] { return t.restart_limit_reached; }));
  wr.Add<uint64_t>(root + "/restart_limit_hits",
                   std::function<uint64_t()>([&t] { return t.restart_limit_hit_count; }));
  wr.Add<uint64_t>(root + "/liveness_failures",
                   std::function<uint64_t()>([&t] { return t.liveness_failures; }));
  wr.Add<uint64_t>(root + "/health_checks",
                   std::function<uint64_t()>([&t] { return t.health_check_count; }));
  wr.Add<uint64_t>(root + "/health_failures",
                   std::function<uint64_t()>([&t] { return t.health_failure_count; }));
  wr.Add<uint64_t>(root + "/manager_health_failures",
                   std::function<uint64_t()>([&t] { return t.manager_health_failure_count; }));
  wr.Add<uint64_t>(root + "/manager_health_timeouts",
                   std::function<uint64_t()>([&t] { return t.manager_health_timeout_count; }));
  wr.Add<uint64_t>(root + "/heartbeat_sent",
                   std::function<uint64_t()>([&t] { return t.heartbeat_sent_count; }));
  wr.Add<int>(root + "/heartbeat_timeout_ms",
              std::function<int()>([&t] { return t.heartbeat_timeout_ms; }));
  wr.Add<uint64_t>(root + "/heartbeat_acks",
                   std::function<uint64_t()>([&t] { return t.heartbeat_ack_count; }));
  wr.Add<int64_t>(root + "/heartbeat_last_ack_age_ms",
                  std::function<int64_t()>(
                      [this, &t] { return HeartbeatLastAckAgeMsForWatcher(t); }));
  wr.Add<uint64_t>(root + "/heartbeat_failures",
                   std::function<uint64_t()>([&t] { return t.heartbeat_failure_count; }));
  wr.Add<uint64_t>(root + "/heartbeat_timeouts",
                   std::function<uint64_t()>([&t] { return t.heartbeat_timeout_count; }));
  wr.Add<uint64_t>(root + "/heartbeat_last_game_time",
                   std::function<uint64_t()>([&t] { return t.heartbeat_last_game_time; }));
  wr.Add<uint64_t>(root + "/heartbeat_snapshot_saves",
                   std::function<uint64_t()>([&t] { return t.heartbeat_snapshot_saves; }));
  wr.Add<uint64_t>(root + "/heartbeat_snapshot_failures",
                   std::function<uint64_t()>([&t] { return t.heartbeat_snapshot_failures; }));
  wr.Add<bool>(root + "/heartbeat_snapshot_dirty",
               std::function<bool()>([&t] { return t.heartbeat_snapshot_dirty; }));
  wr.Add<bool>(root + "/heartbeat_snapshot_save_stale",
               std::function<bool()>([&t] { return t.heartbeat_snapshot_save_stale; }));
  wr.Add<std::string>(root + "/heartbeat_snapshot_status",
                      std::function<std::string()>(
                          [this, &t] { return TargetHeartbeatSnapshotStatus(t); }));
  wr.Add<uint64_t>(root + "/forced_terminations",
                   std::function<uint64_t()>([&t] { return t.forced_termination_count; }));
  wr.Add<uint64_t>(root + "/registry_audits",
                   std::function<uint64_t()>([&t] { return t.registry_audit_count; }));
  wr.Add<uint64_t>(root + "/registry_missing",
                   std::function<uint64_t()>([&t] { return t.registry_missing_count; }));
  wr.Add<std::string>(root + "/last_error",
                      std::function<std::string()>([&t] { return t.last_error; }));
}

void Reviver::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  RegisterTargetWatchers(cellappmgr_target_);
  RegisterTargetWatchers(baseappmgr_target_);

  // Per-target leader lock surface. cellappmgr keeps the legacy
  // reviver/leader/* path for backward compatibility with existing verify
  // scripts; baseappmgr lives under reviver/baseappmgr/leader/*.
  auto& wr = GetWatcherRegistry();
  wr.Add<bool>("reviver/leader/active",
               std::function<bool()>([this] { return HasLeadership(cellappmgr_target_); }));
  wr.Add<std::string>("reviver/leader/lock_path",
                      std::function<std::string()>(
                          [this] { return cellappmgr_target_.leader_lock_path.string(); }));
  wr.Add<uint64_t>("reviver/leader/acquire_count",
                   std::function<uint64_t()>(
                       [this] { return cellappmgr_target_.leader_lock_acquires; }));
  wr.Add<uint64_t>("reviver/leader/acquire_failures",
                   std::function<uint64_t()>(
                       [this] { return cellappmgr_target_.leader_lock_failures; }));

  wr.Add<bool>("reviver/baseappmgr/leader/active",
               std::function<bool()>([this] { return HasLeadership(baseappmgr_target_); }));
  wr.Add<std::string>("reviver/baseappmgr/leader/lock_path",
                      std::function<std::string()>(
                          [this] { return baseappmgr_target_.leader_lock_path.string(); }));
  wr.Add<uint64_t>("reviver/baseappmgr/leader/acquire_count",
                   std::function<uint64_t()>(
                       [this] { return baseappmgr_target_.leader_lock_acquires; }));
  wr.Add<uint64_t>("reviver/baseappmgr/leader/acquire_failures",
                   std::function<uint64_t()>(
                       [this] { return baseappmgr_target_.leader_lock_failures; }));
}

void Reviver::OnTickComplete() {
  for (ManagedTarget* tp : {&cellappmgr_target_, &baseappmgr_target_}) {
    if (!TargetEnabled(*tp)) continue;
    AuditLeadership(*tp);
    if (!HasLeadership(*tp)) continue;
    AuditColdStart(*tp);
    AuditTargetLaunch(*tp);
    AuditTargetLiveness(*tp);
    AuditTargetHeartbeat(*tp);
    AuditTargetHealth(*tp);
    AuditTargetRegistry(*tp);
  }
}

void Reviver::OnTargetBirth(ManagedTarget& t, const machined::BirthNotification& msg) {
  if (!MatchesTargetName(t, msg.name)) return;
  if (t.restart_timer.IsValid()) {
    (void)Dispatcher().CancelTimer(t.restart_timer);
    t.restart_timer = {};
  }
  RememberTarget(t, msg.name, msg.internal_addr, msg.pid);
}

void Reviver::RememberTarget(ManagedTarget& t, std::string_view name, const Address& addr,
                             uint32_t pid) {
  const bool changed = !t.active || t.last_addr != addr || t.last_pid != pid;
  if (!changed) return;
  if (t.last_addr.Port() != 0) Network().DisconnectChannel(t.last_addr);
  if (addr != t.last_addr && addr.Port() != 0) Network().DisconnectChannel(addr);
  ++t.active_generation;
  t.active = true;
  t.last_addr = addr;
  t.last_pid = pid;
  ResetTargetLaunch(t);
  ResetTargetManagerHealth(t);
  ResetTargetHeartbeat(t);
  t.heartbeat_failure_streak = 0;
  t.manager_health_failure_streak = 0;
  t.registry_missing_streak = 0;
  t.heartbeat_last_game_time = 0;
  t.heartbeat_snapshot_saves = 0;
  t.heartbeat_snapshot_failures = 0;
  t.heartbeat_snapshot_dirty = false;
  t.heartbeat_snapshot_save_stale = false;
  t.last_error.clear();
  ATLAS_LOG_INFO("Reviver: {} active name={} pid={} addr={}", t.slug, name, pid, addr.ToString());
}

void Reviver::OnTargetDeath(ManagedTarget& t, const machined::DeathNotification& msg) {
  if (!MatchesTargetName(t, msg.name)) return;
  t.active = false;
  ResetTargetManagerHealth(t);
  ResetTargetHeartbeat(t);
  t.last_addr = msg.internal_addr;
  if (msg.reason == 0) {
    ATLAS_LOG_INFO("Reviver: {} death name={} reason={} addr={}", t.slug, msg.name, msg.reason,
                   msg.internal_addr.ToString());
  } else {
    ATLAS_LOG_WARNING("Reviver: {} death name={} reason={} addr={}", t.slug, msg.name, msg.reason,
                      msg.internal_addr.ToString());
  }
  if (!HasLeadership(t)) return;
  if (msg.reason == 0) return;
  ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::AuditLeadership(ManagedTarget& t) {
  if (HasLeadership(t)) return;
  const auto now = Clock::now();
  if (t.next_leader_lock_attempt != TimePoint{} && now < t.next_leader_lock_attempt) return;
  t.next_leader_lock_attempt = now + std::chrono::seconds(1);

  auto lock = fs::ScopedFileLock::TryAcquire(t.leader_lock_path, LeaderLockContent());
  if (!lock) {
    ++t.leader_lock_failures;
    if (lock.Error().Code() != ErrorCode::kAlreadyExists) {
      t.last_error = std::format("leader lock failed: {}", lock.Error().Message());
      ATLAS_LOG_WARNING("Reviver: {}", t.last_error);
    }
    return;
  }

  t.leader_lock.emplace(std::move(*lock));
  ++t.leader_lock_acquires;
  t.startup_checked = false;
  t.startup_check_at = Clock::now();
  t.last_error.clear();
  ATLAS_LOG_INFO("Reviver: acquired {} leader lock {}", t.slug, t.leader_lock_path.string());
}

void Reviver::AuditColdStart(ManagedTarget& t) {
  if (t.startup_checked || Clock::now() < t.startup_check_at) return;
  if (!t.on_start || t.active) {
    t.startup_checked = true;
    return;
  }
  if (t.query_pending) return;
  t.startup_checked = true;
  t.query_pending = true;
  GetMachinedClient().QueryAsync(t.process_type,
                                 [this, &t](std::vector<machined::ProcessInfo> infos) {
    t.query_pending = false;
    if (t.active) return;
    for (const auto& info : infos) {
      if (!MatchesTargetName(t, info.name)) continue;
      RememberTarget(t, info.name, info.internal_addr, info.pid);
      return;
    }
    ScheduleTargetRestart(t, Duration::zero());
  });
}

void Reviver::AuditTargetRegistry(ManagedTarget& t) {
  if (!t.active || t.query_pending) return;
  if (!GetMachinedClient().IsConnected()) return;
  if (t.audit_interval_ms <= 0) return;
  const auto now = Clock::now();
  if (t.next_registry_audit_at != TimePoint{} && now < t.next_registry_audit_at) return;
  t.next_registry_audit_at = now + Milliseconds(t.audit_interval_ms);
  t.query_pending = true;
  ++t.registry_audit_count;
  GetMachinedClient().QueryAsync(t.process_type,
                                 [this, &t](std::vector<machined::ProcessInfo> infos) {
    OnTargetRegistryAudit(t, std::move(infos));
  });
}

void Reviver::AuditTargetLaunch(ManagedTarget& t) {
  if (!t.launch_pending || t.active) return;
  if (Clock::now() < t.launch_deadline) return;

  t.launch_pending = false;
  t.launch_deadline = {};
  ++t.launch_timeout_count;
  ++t.launch_failures;
  t.last_error = std::format("{} launch pid {} did not register", t.slug, t.launched_pid);
  ATLAS_LOG_WARNING("Reviver: {}", t.last_error);
  TerminateLaunchedTarget(t, "launch registration timeout");
  ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::AuditTargetLiveness(ManagedTarget& t) {
  if (!t.active || t.last_pid == 0) return;
  if (!CanCheckLocalPid()) return;
  // Local pid liveness covers the case where the target died but machined
  // didn't (or hasn't yet) routed us a death notification.
  if (t.last_pid != t.launched_pid && !IsLoopbackAddress(t.last_addr)) return;
  if (IsProcessAlive(t.last_pid)) return;

  t.active = false;
  ResetTargetManagerHealth(t);
  ResetTargetHeartbeat(t);
  ++t.liveness_failures;
  t.last_error = std::format("{} pid {} is no longer alive", t.slug, t.last_pid);
  ATLAS_LOG_WARNING("Reviver: {}", t.last_error);
  ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::SendHeartbeat(ManagedTarget& t) {
  // ConnectRudpNocwnd is idempotent for live addrs — returns the cached
  // ReliableUdpChannel; rebuilds only after a real disconnect/eviction.
  auto ch = Network().ConnectRudpNocwnd(t.last_addr);
  if (!ch || *ch == nullptr) {
    RecordTargetHeartbeatFailure(t, "heartbeat channel failed");
    return;
  }
  const uint64_t nonce = ++t.heartbeat_nonce;
  t.heartbeat_pending_nonce = nonce;
  t.heartbeat_pending = true;
  t.heartbeat_sent_at = Clock::now();
  ++t.heartbeat_sent_count;

  if (t.process_type == ProcessType::kCellAppMgr) {
    cellappmgr::HealthProbe msg;
    msg.nonce = nonce;
    if (auto send = (*ch)->SendMessage(msg); !send) {
      t.heartbeat_pending = false;
      t.heartbeat_pending_nonce = 0;
      RecordTargetHeartbeatFailure(t, "heartbeat send failed");
    }
  } else {
    baseappmgr::HealthProbe msg;
    msg.nonce = nonce;
    if (auto send = (*ch)->SendMessage(msg); !send) {
      t.heartbeat_pending = false;
      t.heartbeat_pending_nonce = 0;
      RecordTargetHeartbeatFailure(t, "heartbeat send failed");
    }
  }
}

void Reviver::AuditTargetHeartbeat(ManagedTarget& t) {
  if (!t.active) return;
  if (t.health_interval_ms <= 0) return;
  const auto now = Clock::now();
  const auto interval = Milliseconds(t.health_interval_ms);
  const int timeout_ms =
      t.heartbeat_timeout_ms > 0 ? t.heartbeat_timeout_ms : std::max(500, t.health_interval_ms * 2);
  const auto timeout = Milliseconds(timeout_ms);

  if (t.heartbeat_pending) {
    if (now - t.heartbeat_sent_at < timeout) return;
    t.heartbeat_pending = false;
    t.heartbeat_pending_nonce = 0;
    t.next_heartbeat_at = now + interval;
    ++t.heartbeat_timeout_count;
    RecordTargetHeartbeatFailure(t, "heartbeat timed out");
    return;
  }

  if (t.next_heartbeat_at != TimePoint{} && now < t.next_heartbeat_at) return;
  t.next_heartbeat_at = now + interval;
  SendHeartbeat(t);
}

void Reviver::RecordHeartbeatAck(ManagedTarget& t, const Address& src, uint64_t nonce,
                                 uint64_t game_time, uint64_t snapshot_saves,
                                 uint64_t snapshot_failures, bool snapshot_dirty,
                                 bool snapshot_save_stale) {
  if (!t.active) return;
  if (src != t.last_addr) return;
  if (!t.heartbeat_pending || nonce != t.heartbeat_pending_nonce) return;
  t.heartbeat_pending = false;
  t.heartbeat_pending_nonce = 0;
  t.heartbeat_failure_streak = 0;
  t.restart_attempts = 0;
  t.restart_limit_reached = false;
  t.last_error.clear();
  t.heartbeat_last_ack_at = Clock::now();
  t.heartbeat_last_game_time = game_time;
  t.heartbeat_snapshot_saves = snapshot_saves;
  t.heartbeat_snapshot_failures = snapshot_failures;
  t.heartbeat_snapshot_dirty = snapshot_dirty;
  t.heartbeat_snapshot_save_stale = snapshot_save_stale;
  ++t.heartbeat_ack_count;
}

void Reviver::OnCellAppMgrHeartbeatAck(const Address& src, Channel*,
                                       const cellappmgr::HealthProbeAck& msg) {
  RecordHeartbeatAck(cellappmgr_target_, src, msg.nonce, msg.game_time, msg.snapshot_saves,
                     msg.snapshot_failures, msg.snapshot_dirty, msg.snapshot_save_stale);
}

void Reviver::OnBaseAppMgrHeartbeatAck(const Address& src, Channel*,
                                       const baseappmgr::HealthProbeAck& msg) {
  RecordHeartbeatAck(baseappmgr_target_, src, msg.nonce, msg.game_time, msg.snapshot_saves,
                     msg.snapshot_failures, msg.snapshot_dirty, msg.snapshot_save_stale);
}

void Reviver::AuditTargetHealth(ManagedTarget& t) {
  if (!t.active) return;
  if (t.health_interval_ms <= 0) return;
  const auto now = Clock::now();
  if (t.health_pending) {
    const auto timeout_ms = std::max(1, t.manager_health_timeout_ms);
    const auto timeout = Milliseconds(timeout_ms);
    if (t.health_check_sent_at != TimePoint{} && now - t.health_check_sent_at < timeout) {
      return;
    }
    t.health_pending = false;
    t.health_check_sent_at = {};
    ++t.health_check_generation;
    ++t.manager_health_timeout_count;
    RecordTargetManagerHealthFailure(t, "manager health watcher timed out");
    return;
  }
  if (!GetMachinedClient().IsConnected()) return;
  if (t.next_health_check_at != TimePoint{} && now < t.next_health_check_at) return;
  t.next_health_check_at = now + Milliseconds(t.health_interval_ms);
  t.health_pending = true;
  t.health_check_sent_at = now;
  ++t.health_check_count;
  const uint64_t generation = ++t.health_check_generation;
  GetMachinedClient().QueryWatcher(
      t.process_type, t.configured_name, "app/uptime_seconds",
      [this, &t, generation](bool found, const std::string&, const std::string&) {
        OnTargetHealth(t, generation, found);
      });
}

void Reviver::OnTargetHealth(ManagedTarget& t, uint64_t generation, bool found) {
  if (generation != t.health_check_generation) return;
  t.health_pending = false;
  t.health_check_sent_at = {};
  if (!t.active) return;
  if (found) {
    t.manager_health_failure_streak = 0;
    return;
  }
  RecordTargetManagerHealthFailure(t, "manager health watcher did not respond");
}

void Reviver::OnTargetRegistryAudit(ManagedTarget& t, std::vector<machined::ProcessInfo> infos) {
  t.query_pending = false;
  if (!t.active) return;
  for (const auto& info : infos) {
    if (!MatchesTargetName(t, info.name)) continue;
    if (t.last_pid != info.pid || t.last_addr != info.internal_addr) {
      RememberTarget(t, info.name, info.internal_addr, info.pid);
    } else {
      t.registry_missing_streak = 0;
    }
    return;
  }

  ++t.registry_missing_streak;
  ++t.registry_missing_count;
  const auto threshold = static_cast<uint32_t>(std::max(1, t.missing_audit_threshold));
  if (t.registry_missing_streak < threshold) return;

  t.active = false;
  ResetTargetManagerHealth(t);
  ResetTargetHeartbeat(t);
  TerminateTargetIfLocal(t, "registry missing");
  t.last_error = std::format("{} missing from machined registry after {} audit(s)", t.slug,
                             t.registry_missing_streak);
  ATLAS_LOG_WARNING("Reviver: {}", t.last_error);
  ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::RecordTargetHeartbeatFailure(ManagedTarget& t, std::string_view reason) {
  ++t.heartbeat_failure_count;
  if (t.last_addr.Port() != 0) Network().DisconnectChannel(t.last_addr);
  RecordTargetHealthFailure(t, reason, t.heartbeat_failure_streak);
}

void Reviver::RecordTargetManagerHealthFailure(ManagedTarget& t, std::string_view reason) {
  ++t.manager_health_failure_count;
  RecordTargetHealthFailure(t, reason, t.manager_health_failure_streak);
}

void Reviver::RecordTargetHealthFailure(ManagedTarget& t, std::string_view reason,
                                        uint32_t& streak) {
  ++streak;
  ++t.health_failure_count;
  const auto threshold = static_cast<uint32_t>(std::max(1, t.health_failure_threshold));
  if (streak < threshold) return;

  t.active = false;
  ResetTargetManagerHealth(t);
  ResetTargetHeartbeat(t);
  TerminateTargetIfLocal(t, reason);
  t.last_error = std::format("{} after {} check(s)", reason, streak);
  ATLAS_LOG_WARNING("Reviver: {}", t.last_error);
  ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::ResetTargetHeartbeat(ManagedTarget& t) {
  t.heartbeat_pending = false;
  t.heartbeat_pending_nonce = 0;
  t.heartbeat_sent_at = {};
  t.next_heartbeat_at = {};
  t.heartbeat_last_ack_at = {};
}

void Reviver::ResetTargetManagerHealth(ManagedTarget& t) {
  t.health_pending = false;
  t.health_check_sent_at = {};
  t.next_health_check_at = {};
  ++t.health_check_generation;
}

void Reviver::ResetTargetLaunch(ManagedTarget& t) {
  t.launch_pending = false;
  t.launch_deadline = {};
}

void Reviver::TerminateTargetIfLocal(ManagedTarget& t, std::string_view reason) {
  if (t.last_pid == 0 || !CanCheckLocalPid()) return;
  if (t.last_pid != t.launched_pid && !IsLoopbackAddress(t.last_addr)) return;
  if (!IsProcessAlive(t.last_pid)) return;
  if (!TerminateProcessByPid(t.last_pid)) return;
  ++t.forced_termination_count;
  ATLAS_LOG_WARNING("Reviver: terminated {} pid={} reason={}", t.slug, t.last_pid, reason);
}

void Reviver::TerminateLaunchedTarget(ManagedTarget& t, std::string_view reason) {
  if (t.launched_pid == 0) return;
  if (!IsProcessAlive(t.launched_pid)) return;
  if (!TerminateProcessByPid(t.launched_pid)) return;
  ++t.forced_termination_count;
  ATLAS_LOG_WARNING("Reviver: terminated launched {} pid={} reason={}", t.slug, t.launched_pid,
                    reason);
}

void Reviver::ScheduleTargetRestart(ManagedTarget& t, Duration delay) {
  if (!HasLeadership(t)) return;
  if (t.restart_timer.IsValid()) return;
  if (t.restart_attempts >= static_cast<uint32_t>(std::max(0, Config().revive_max_restarts))) {
    if (!t.restart_limit_reached) ++t.restart_limit_hit_count;
    t.restart_limit_reached = true;
    t.last_error = "restart limit reached";
    ATLAS_LOG_ERROR("Reviver: {} restart limit reached", t.slug);
    return;
  }
  const int cap_ms = Config().revive_restart_backoff_cap_ms;
  if (cap_ms > 0 && delay > Duration::zero()) {
    const auto base_ms = std::chrono::duration_cast<Milliseconds>(delay).count();
    if (base_ms > 0) {
      const auto shift = std::min<uint32_t>(t.restart_attempts, 16);
      const int64_t scaled = base_ms << shift;
      const auto bounded = std::min<int64_t>(scaled, cap_ms);
      delay = std::chrono::duration_cast<Duration>(Milliseconds(bounded));
    }
  }
  t.restart_timer = Dispatcher().AddTimer(delay, [this, &t](TimerHandle) {
    t.restart_timer = {};
    LaunchTarget(t);
  });
}

void Reviver::LaunchTarget(ManagedTarget& t) {
  if (!HasLeadership(t)) return;
  if (t.active) return;
  ++t.restart_attempts;
  // Resolve exe relative to the Reviver process when no explicit path was
  // configured (matches the legacy CellAppMgr behaviour).
  std::filesystem::path exe = t.exe;
  if (exe.empty()) {
#if defined(_WIN32)
    const auto kExeName = t.process_type == ProcessType::kCellAppMgr ? "atlas_cellappmgr.exe"
                                                                      : "atlas_baseappmgr.exe";
#else
    const auto kExeName = t.process_type == ProcessType::kCellAppMgr ? "atlas_cellappmgr"
                                                                      : "atlas_baseappmgr";
#endif
    exe = !self_exe_.empty() ? self_exe_.parent_path() / kExeName : std::filesystem::path{kExeName};
  } else {
    exe = std::filesystem::absolute(exe);
  }

  const uint16_t port =
      t.internal_port != 0 ? t.internal_port : t.last_addr.Port();

  if (exe.empty() || !std::filesystem::exists(exe)) {
    ++t.launch_failures;
    t.last_error = std::format("{} exe not found: {}", t.slug, exe.string());
    ATLAS_LOG_ERROR("Reviver: {}", t.last_error);
    ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
    return;
  }
  if (port == 0) {
    ++t.launch_failures;
    t.last_error = std::format("{} internal port is not configured", t.slug);
    ATLAS_LOG_ERROR("Reviver: {}", t.last_error);
    ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
    return;
  }

  std::vector<std::string> args;
  if (!Config().config_path.empty()) {
    args.push_back("--config");
    args.push_back(Config().config_path.string());
  }
  args.insert(args.end(), {
      "--type", t.process_type == ProcessType::kCellAppMgr ? "cellappmgr" : "baseappmgr",
      "--name", t.configured_name,
      "--internal-port", std::to_string(port),
      "--machined", Config().machined_address.ToString(),
      "--update-hertz", std::to_string(t.update_hertz)});
  const auto snapshot_path = t.snapshot_path.empty() ? Config().snapshot_path : t.snapshot_path;
  if (!snapshot_path.empty()) {
    args.push_back("--snapshot-path");
    args.push_back(snapshot_path.string());
    const int snapshot_interval_ms =
        t.snapshot_interval_ms >= 0 ? t.snapshot_interval_ms : Config().snapshot_interval_ms;
    args.push_back("--snapshot-interval-ms");
    args.push_back(std::to_string(std::max(0, snapshot_interval_ms)));
  }

  ProcessLaunchOptions opts;
  opts.exe = exe;
  opts.args = std::move(args);
  opts.working_directory = exe.parent_path();
  opts.output_path = t.output_path;
  auto pid = LaunchDetachedProcess(std::move(opts));
  if (!pid) {
    ++t.launch_failures;
    t.last_error = pid.Error().Message();
    ATLAS_LOG_ERROR("Reviver: failed to launch {}: {}", t.slug, t.last_error);
    ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
    return;
  }

  t.launched_pid = *pid;
  t.launch_pending = true;
  t.launch_deadline = Clock::now() + Milliseconds(std::max(1, t.launch_timeout_ms));
  ++t.launch_count;
  ATLAS_LOG_WARNING("Reviver: launched {} attempt={} pid={} exe={} port={}", t.slug,
                    t.restart_attempts, t.launched_pid, exe.string(), port);
}

auto Reviver::HasLeadership(const ManagedTarget& t) const -> bool {
  return t.leader_lock.has_value() && t.leader_lock->IsHeld();
}

auto Reviver::MatchesTargetName(const ManagedTarget& t, std::string_view name) const -> bool {
  return t.configured_name.empty() || name == t.configured_name;
}

auto Reviver::CanCheckLocalPid() const -> bool {
  // Liveness probes only make sense for processes the Reviver itself
  // launched OR processes on loopback (single-host development).
  return true;
}

auto Reviver::ResolveLeaderLockPath(const ManagedTarget& t) const -> std::filesystem::path {
  if (!t.leader_lock_path.empty()) {
    return std::filesystem::absolute(t.leader_lock_path);
  }
  // cellappmgr keeps the legacy --revive-leader-lock-path top-level option
  // for backwards compatibility with existing verify scripts. baseappmgr
  // requires --revive-baseappmgr-leader-lock-path (or the snapshot dir).
  if (t.process_type == ProcessType::kCellAppMgr && !Config().revive_leader_lock_path.empty()) {
    return std::filesystem::absolute(Config().revive_leader_lock_path);
  }
  const auto target_name = SanitizeLockSegment(t.configured_name);
  const uint16_t port = t.internal_port;
  auto base = t.snapshot_path.parent_path();
  if (base.empty()) base = Config().snapshot_path.parent_path();
  if (base.empty()) base = fs::TempDirectory();
  return base / std::format("atlas_reviver_{}_{}.lock", target_name, port);
}

auto Reviver::LeaderLockContent() const -> std::string {
  return std::format("process={} pid={}\n", Config().process_name, CurrentPid());
}

auto Reviver::TargetStatus(const ManagedTarget& t) const -> std::string {
  if (!HasLeadership(t)) return "standby";
  if (t.restart_limit_reached) return "restart_limited";
  if (t.active) return "active";
  if (t.launch_pending) return "launching";
  if (t.restart_timer.IsValid()) return "restart_scheduled";
  if (t.query_pending) return "querying";
  return "idle";
}

auto Reviver::HeartbeatLastAckAgeMsForWatcher(const ManagedTarget& t) const -> int64_t {
  return AgeMsSince(t.heartbeat_last_ack_at);
}

auto Reviver::TargetHeartbeatSnapshotStatus(const ManagedTarget& t) const -> std::string {
  const char* state = "unknown";
  if (!t.active) {
    state = "inactive";
  } else if (t.heartbeat_last_ack_at.time_since_epoch() == Duration::zero()) {
    state = "unknown";
  } else if (t.heartbeat_snapshot_failures > 0) {
    state = "failed";
  } else if (t.heartbeat_snapshot_dirty) {
    state = "dirty";
  } else if (t.heartbeat_snapshot_save_stale) {
    state = "stale";
  } else {
    state = "ready";
  }
  return std::format("state={} saves={} failures={} dirty={} stale={} game_time={} "
                     "ack_age_ms={}",
                     state, t.heartbeat_snapshot_saves, t.heartbeat_snapshot_failures,
                     t.heartbeat_snapshot_dirty ? 1 : 0,
                     t.heartbeat_snapshot_save_stale ? 1 : 0, t.heartbeat_last_game_time,
                     HeartbeatLastAckAgeMsForWatcher(t));
}

}  // namespace atlas
