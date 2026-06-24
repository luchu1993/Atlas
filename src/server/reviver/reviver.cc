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
#include "dbappmgr/dbappmgr_messages.h"
#include "foundation/log.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "server/machined_client.h"
#include "server/watcher.h"

namespace atlas {

namespace {

auto IsLoopbackAddress(const Address& addr) -> bool {
  const uint32_t ip = addr.Ip();
  const auto* bytes = reinterpret_cast<const uint8_t*>(&ip);
  return bytes[0] == 127;
}

auto AgeMsSince(TimePoint t) -> int64_t {
  if (t.time_since_epoch() == Duration::zero()) return -1;
  return std::max<int64_t>(0, std::chrono::duration_cast<Milliseconds>(Clock::now() - t).count());
}

auto TargetExeName(ProcessType process_type) -> std::string_view {
  switch (process_type) {
#if defined(_WIN32)
    case ProcessType::kCellAppMgr:
      return "atlas_cellappmgr.exe";
    case ProcessType::kBaseAppMgr:
      return "atlas_baseappmgr.exe";
    case ProcessType::kDbAppMgr:
      return "atlas_dbappmgr.exe";
#else
    case ProcessType::kCellAppMgr:
      return "atlas_cellappmgr";
    case ProcessType::kBaseAppMgr:
      return "atlas_baseappmgr";
    case ProcessType::kDbAppMgr:
      return "atlas_dbappmgr";
#endif
    default:
      return {};
  }
}

auto TargetTypeArg(ProcessType process_type) -> std::string_view {
  switch (process_type) {
    case ProcessType::kCellAppMgr:
      return "cellappmgr";
    case ProcessType::kBaseAppMgr:
      return "baseappmgr";
    case ProcessType::kDbAppMgr:
      return "dbappmgr";
    default:
      return {};
  }
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

void Reviver::InitTarget(ManagedTarget& t) {
  // t.process_type / t.slug pre-set by Init() pre ManagerApp::Init.
  if (t.process_type == ProcessType::kCellAppMgr) {
    t.configured_name = Config().revive_cellappmgr_name;
    t.exe = Config().revive_cellappmgr_exe;
    t.internal_port = Config().revive_cellappmgr_internal_port;
    t.output_path = Config().revive_cellappmgr_output_path;
    t.update_hertz = Config().revive_cellappmgr_update_hertz;
    t.launch_timeout_ms = Config().revive_cellappmgr_launch_timeout_ms;
    t.health_interval_ms = Config().revive_cellappmgr_health_interval_ms;
    t.heartbeat_timeout_ms = Config().revive_cellappmgr_heartbeat_timeout_ms;
    t.manager_health_timeout_ms = Config().revive_cellappmgr_manager_health_timeout_ms;
    t.health_failure_threshold = Config().revive_cellappmgr_health_failure_threshold;
    t.audit_interval_ms = Config().revive_cellappmgr_audit_interval_ms;
    t.missing_audit_threshold = Config().revive_cellappmgr_missing_audit_threshold;
    t.on_start = Config().revive_cellappmgr_on_start;
    t.priority = static_cast<uint8_t>(std::clamp(Config().revive_cellappmgr_priority, 0, 255));
  } else if (t.process_type == ProcessType::kBaseAppMgr) {
    t.configured_name = Config().revive_baseappmgr_name;
    t.exe = Config().revive_baseappmgr_exe;
    t.internal_port = Config().revive_baseappmgr_internal_port;
    t.output_path = Config().revive_baseappmgr_output_path;
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
    t.priority = static_cast<uint8_t>(std::clamp(Config().revive_baseappmgr_priority, 0, 255));
  } else {
    t.configured_name = Config().revive_dbappmgr_name;
    t.exe = Config().revive_dbappmgr_exe;
    t.internal_port = Config().revive_dbappmgr_internal_port;
    t.output_path = Config().revive_dbappmgr_output_path;
    t.update_hertz = Config().revive_dbappmgr_update_hertz;
    t.launch_timeout_ms = Config().revive_dbappmgr_launch_timeout_ms;
    t.health_interval_ms = Config().revive_cellappmgr_health_interval_ms;
    t.heartbeat_timeout_ms = Config().revive_cellappmgr_heartbeat_timeout_ms;
    t.manager_health_timeout_ms = Config().revive_cellappmgr_manager_health_timeout_ms;
    t.health_failure_threshold = Config().revive_cellappmgr_health_failure_threshold;
    t.audit_interval_ms = Config().revive_cellappmgr_audit_interval_ms;
    t.missing_audit_threshold = Config().revive_cellappmgr_missing_audit_threshold;
    t.on_start = Config().revive_dbappmgr_on_start;
    t.priority = static_cast<uint8_t>(std::clamp(Config().revive_dbappmgr_priority, 0, 255));
  }
  // Seed the takeover grace from startup so a lone Reviver cold-starts an
  // absent subject after its priority-scaled delay rather than never.
  if (TargetEnabled(t)) t.subject_down_at = Clock::now();
}

auto Reviver::TargetEnabled(const ManagedTarget& t) const -> bool {
  // Empty everything → silently disabled so tests can supervise just one
  // target; explicit on_start / exe / port flips it on.
  if (t.on_start) return true;
  if (!t.exe.empty()) return true;
  if (t.internal_port != 0) return true;
  return false;
}

auto Reviver::Init(int argc, char* argv[]) -> bool {
  if (argc > 0 && argv[0] != nullptr) self_exe_ = std::filesystem::absolute(argv[0]);
  // ManagerApp::Init dispatches into RegisterTargetWatchers which bakes
  // t.slug into the watcher path; must be set before that runs.
  cellappmgr_target_.slug = "cellappmgr";
  cellappmgr_target_.process_type = ProcessType::kCellAppMgr;
  baseappmgr_target_.slug = "baseappmgr";
  baseappmgr_target_.process_type = ProcessType::kBaseAppMgr;
  dbappmgr_target_.slug = "dbappmgr";
  dbappmgr_target_.process_type = ProcessType::kDbAppMgr;
  if (!ManagerApp::Init(argc, argv)) return false;

  InitTarget(cellappmgr_target_);
  InitTarget(baseappmgr_target_);
  InitTarget(dbappmgr_target_);

  cellappmgr_target_.startup_check_at = Clock::now() + std::chrono::milliseconds(500);
  baseappmgr_target_.startup_check_at = Clock::now() + std::chrono::milliseconds(500);
  dbappmgr_target_.startup_check_at = Clock::now() + std::chrono::milliseconds(500);

  if (TargetEnabled(cellappmgr_target_)) {
    GetMachinedClient().Subscribe(
        machined::ListenerType::kBoth, ProcessType::kCellAppMgr,
        [this](const machined::BirthNotification& msg) { OnTargetBirth(cellappmgr_target_, msg); },
        [this](const machined::DeathNotification& msg) { OnTargetDeath(cellappmgr_target_, msg); });
    (void)Network().InterfaceTable().RegisterTypedHandler<cellappmgr::HealthProbeAck>(
        [this](const Address& src, Channel* ch, const cellappmgr::HealthProbeAck& msg) {
          OnCellAppMgrHeartbeatAck(src, ch, msg);
        });
  }
  if (TargetEnabled(baseappmgr_target_)) {
    GetMachinedClient().Subscribe(
        machined::ListenerType::kBoth, ProcessType::kBaseAppMgr,
        [this](const machined::BirthNotification& msg) { OnTargetBirth(baseappmgr_target_, msg); },
        [this](const machined::DeathNotification& msg) { OnTargetDeath(baseappmgr_target_, msg); });
    (void)Network().InterfaceTable().RegisterTypedHandler<baseappmgr::HealthProbeAck>(
        [this](const Address& src, Channel* ch, const baseappmgr::HealthProbeAck& msg) {
          OnBaseAppMgrHeartbeatAck(src, ch, msg);
        });
  }
  if (TargetEnabled(dbappmgr_target_)) {
    GetMachinedClient().Subscribe(
        machined::ListenerType::kBoth, ProcessType::kDbAppMgr,
        [this](const machined::BirthNotification& msg) { OnTargetBirth(dbappmgr_target_, msg); },
        [this](const machined::DeathNotification& msg) { OnTargetDeath(dbappmgr_target_, msg); });
    (void)Network().InterfaceTable().RegisterTypedHandler<dbappmgr::HealthProbeAck>(
        [this](const Address& src, Channel* ch, const dbappmgr::HealthProbeAck& msg) {
          OnDbAppMgrHeartbeatAck(src, ch, msg);
        });
  }
  return true;
}

void Reviver::Fini() {
  for (ManagedTarget* tp : {&cellappmgr_target_, &baseappmgr_target_, &dbappmgr_target_}) {
    if (tp->restart_timer.IsValid()) {
      (void)Dispatcher().CancelTimer(tp->restart_timer);
      tp->restart_timer = {};
    }
  }
  ManagerApp::Fini();
}

void Reviver::RegisterTargetWatchers(ManagedTarget& t) {
  auto& wr = GetWatcherRegistry();
  const auto root = std::format("reviver/{}", t.slug);
  wr.Add<std::string>(root + "/status",
                      std::function<std::string()>([this, &t] { return TargetStatus(t); }));
  wr.Add<bool>(root + "/active", std::function<bool()>([&t] { return t.active; }));
  wr.Add<uint32_t>(root + "/active_pid", std::function<uint32_t()>([&t] { return t.last_pid; }));
  wr.Add<uint64_t>(root + "/active_generation",
                   std::function<uint64_t()>([&t] { return t.active_generation; }));
  wr.Add<uint32_t>(root + "/launched_pid",
                   std::function<uint32_t()>([&t] { return t.launched.Pid(); }));
  wr.Add<std::string>(root + "/output_path",
                      std::function<std::string()>([&t] { return t.output_path.string(); }));
  wr.Add<bool>(root + "/launch_pending", std::function<bool()>([&t] { return t.launch_pending; }));
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
  wr.Add<int64_t>(root + "/heartbeat_last_ack_age_ms", std::function<int64_t()>([this, &t] {
                    return HeartbeatLastAckAgeMsForWatcher(t);
                  }));
  wr.Add<uint64_t>(root + "/heartbeat_failures",
                   std::function<uint64_t()>([&t] { return t.heartbeat_failure_count; }));
  wr.Add<uint64_t>(root + "/heartbeat_timeouts",
                   std::function<uint64_t()>([&t] { return t.heartbeat_timeout_count; }));
  wr.Add<uint64_t>(root + "/heartbeat_last_game_time",
                   std::function<uint64_t()>([&t] { return t.heartbeat_last_game_time; }));
  wr.Add<int>(root + "/priority",
              std::function<int()>([&t] { return static_cast<int>(t.priority); }));
  wr.Add<bool>(root + "/active_reviver",
               std::function<bool()>([&t] { return t.is_active_reviver; }));
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
  RegisterTargetWatchers(dbappmgr_target_);

  // cellappmgr keeps the legacy reviver/leader/* watcher path for verify;
  // other targets live under reviver/<slug>/leader/*.
  auto& wr = GetWatcherRegistry();
  // "active" now means subject-designated active monitor (legacy alias kept so
  // verify scripts that watched the leader-lock flag still resolve).
  wr.Add<bool>("reviver/leader/active",
               std::function<bool()>([this] { return cellappmgr_target_.is_active_reviver; }));
  wr.Add<bool>("reviver/baseappmgr/leader/active",
               std::function<bool()>([this] { return baseappmgr_target_.is_active_reviver; }));
  wr.Add<bool>("reviver/dbappmgr/leader/active",
               std::function<bool()>([this] { return dbappmgr_target_.is_active_reviver; }));
}

void Reviver::OnTickComplete() {
  for (ManagedTarget* tp : {&cellappmgr_target_, &baseappmgr_target_, &dbappmgr_target_}) {
    if (!TargetEnabled(*tp)) continue;
    const bool supervise = ShouldSupervise(*tp);
    if (supervise) {
      AuditRevive(*tp);
      AuditTargetLaunch(*tp);
      AuditTargetLiveness(*tp);
    }
    // Every Reviver pings the subject so it can arbitrate the active monitor
    // (standbys included); after liveness so a dead local pid is caught first.
    AuditTargetHeartbeat(*tp);
    if (supervise) {
      AuditTargetHealth(*tp);
      AuditTargetRegistry(*tp);
    }
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
  t.ever_active = true;
  t.intentional_down = false;
  t.subject_down_at = {};
  t.last_addr = addr;
  t.last_pid = pid;
  ResetTargetLaunch(t);
  ResetTargetManagerHealth(t);
  ResetTargetHeartbeat(t);
  t.heartbeat_failure_streak = 0;
  t.manager_health_failure_streak = 0;
  t.registry_missing_streak = 0;
  t.heartbeat_last_game_time = 0;
  t.last_error.clear();
  ATLAS_LOG_INFO("Reviver: {} active name={} pid={} addr={}", t.slug, name, pid, addr.ToString());
}

void Reviver::OnTargetDeath(ManagedTarget& t, const machined::DeathNotification& msg) {
  if (!MatchesTargetName(t, msg.name)) return;
  if (t.active && t.subject_down_at == TimePoint{}) t.subject_down_at = Clock::now();
  t.active = false;
  t.intentional_down = (msg.reason == 0);
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
  // A graceful exit (reason 0) stays down; AuditRevive resurrects only an
  // unexpectedly-dead subject, gated by ShouldSupervise + registry dedup.
  if (msg.reason == 0) return;
  ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::AuditRevive(ManagedTarget& t) {
  // Called only when ShouldSupervise(t). Revives an absent subject — covers both
  // the initial cold-start and a takeover after the designated Reviver itself
  // died. A registry query right before launch dedups racing Revivers: if a
  // higher-priority survivor already revived it, we adopt it instead.
  if (Clock::now() < t.startup_check_at) return;
  if (t.active || t.intentional_down) return;
  if (t.query_pending || t.launch_pending || t.restart_timer.IsValid()) return;
  if (!t.ever_active && !t.on_start) return;  // never seen + not auto-started
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
  t.last_error = std::format("{} launch pid {} did not register", t.slug, t.launched.Pid());
  ATLAS_LOG_WARNING("Reviver: {}", t.last_error);
  TerminateLaunchedTarget(t, "launch registration timeout");
  ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::AuditTargetLiveness(ManagedTarget& t) {
  if (!t.active || t.last_pid == 0) return;
  if (!CanCheckLocalPid()) return;
  // Catches the gap where the supervised process died but machined hasn't yet
  // routed us a death notification. Use the owning handle when this is OUR
  // launched process (no PID-reuse hazard); fall back to pid-only check for
  // externally-started loopback processes.
  if (t.launched.IsValid() && t.last_pid == t.launched.Pid()) {
    if (t.launched.IsAlive()) return;
  } else if (IsLoopbackAddress(t.last_addr)) {
    if (IsProcessAlive(t.last_pid)) return;
  } else {
    return;
  }

  if (t.subject_down_at == TimePoint{}) t.subject_down_at = Clock::now();
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
    msg.reviver_priority = t.priority;
    if (auto send = (*ch)->SendMessage(msg); !send) {
      t.heartbeat_pending = false;
      t.heartbeat_pending_nonce = 0;
      RecordTargetHeartbeatFailure(t, "heartbeat send failed");
    }
  } else if (t.process_type == ProcessType::kBaseAppMgr) {
    baseappmgr::HealthProbe msg;
    msg.nonce = nonce;
    msg.reviver_priority = t.priority;
    if (auto send = (*ch)->SendMessage(msg); !send) {
      t.heartbeat_pending = false;
      t.heartbeat_pending_nonce = 0;
      RecordTargetHeartbeatFailure(t, "heartbeat send failed");
    }
  } else {
    dbappmgr::HealthProbe msg;
    msg.nonce = nonce;
    msg.reviver_priority = t.priority;
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
                                 uint64_t game_time, bool is_active_reviver) {
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
  t.is_active_reviver = is_active_reviver;
  ++t.heartbeat_ack_count;
}

void Reviver::OnCellAppMgrHeartbeatAck(const Address& src, Channel*,
                                       const cellappmgr::HealthProbeAck& msg) {
  RecordHeartbeatAck(cellappmgr_target_, src, msg.nonce, msg.game_time, msg.is_active_reviver);
}

void Reviver::OnBaseAppMgrHeartbeatAck(const Address& src, Channel*,
                                       const baseappmgr::HealthProbeAck& msg) {
  RecordHeartbeatAck(baseappmgr_target_, src, msg.nonce, msg.game_time, msg.is_active_reviver);
}

void Reviver::OnDbAppMgrHeartbeatAck(const Address& src, Channel*,
                                     const dbappmgr::HealthProbeAck& msg) {
  RecordHeartbeatAck(dbappmgr_target_, src, msg.nonce, msg.game_time, msg.is_active_reviver);
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

  if (t.subject_down_at == TimePoint{}) t.subject_down_at = Clock::now();
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

  // Mark the subject down even on a standby's failed heartbeat — its takeover
  // grace counts from here. Only the supervising Reviver terminates/restarts;
  // standbys defer to AuditRevive once their grace elapses.
  if (t.active) t.subject_down_at = Clock::now();
  t.active = false;
  ResetTargetManagerHealth(t);
  ResetTargetHeartbeat(t);
  t.last_error = std::format("{} after {} check(s)", reason, streak);
  ATLAS_LOG_WARNING("Reviver: {}", t.last_error);
  if (!ShouldSupervise(t)) return;
  TerminateTargetIfLocal(t, reason);
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
  bool killed = false;
  if (t.launched.IsValid() && t.last_pid == t.launched.Pid()) {
    if (!t.launched.IsAlive()) return;
    killed = t.launched.Terminate();
  } else if (IsLoopbackAddress(t.last_addr)) {
    if (!IsProcessAlive(t.last_pid)) return;
    killed = TerminateProcessByPid(t.last_pid);
  } else {
    return;
  }
  if (!killed) return;
  ++t.forced_termination_count;
  ATLAS_LOG_WARNING("Reviver: terminated {} pid={} reason={}", t.slug, t.last_pid, reason);
}

void Reviver::TerminateLaunchedTarget(ManagedTarget& t, std::string_view reason) {
  if (!t.launched.IsValid()) return;
  if (!t.launched.IsAlive()) return;
  const auto pid = t.launched.Pid();
  if (!t.launched.Terminate()) return;
  ++t.forced_termination_count;
  ATLAS_LOG_WARNING("Reviver: terminated launched {} pid={} reason={}", t.slug, pid, reason);
}

void Reviver::ScheduleTargetRestart(ManagedTarget& t, Duration delay) {
  if (!ShouldSupervise(t)) return;
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
  if (!ShouldSupervise(t)) return;
  if (t.active) return;
  ++t.restart_attempts;
  // Resolve exe relative to the Reviver process when no explicit path was
  // configured (matches the legacy CellAppMgr behaviour).
  std::filesystem::path exe = t.exe;
  if (exe.empty()) {
    const auto kExeName = TargetExeName(t.process_type);
    exe = !self_exe_.empty() ? self_exe_.parent_path() / kExeName : std::filesystem::path{kExeName};
  } else {
    exe = std::filesystem::absolute(exe);
  }

  const uint16_t port = t.internal_port != 0 ? t.internal_port : t.last_addr.Port();

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
  args.insert(args.end(), {"--type", std::string(TargetTypeArg(t.process_type)), "--name",
                           t.configured_name, "--internal-port", std::to_string(port), "--machined",
                           Config().machined_address.ToString(), "--update-hertz",
                           std::to_string(t.update_hertz)});

  ProcessLaunchOptions opts;
  opts.exe = exe;
  opts.args = std::move(args);
  opts.working_directory = exe.parent_path();
  opts.output_path = t.output_path;
  auto launched = LaunchDetachedProcess(std::move(opts));
  if (!launched) {
    ++t.launch_failures;
    t.last_error = launched.Error().Message();
    ATLAS_LOG_ERROR("Reviver: failed to launch {}: {}", t.slug, t.last_error);
    ScheduleTargetRestart(t, Milliseconds(Config().revive_restart_delay_ms));
    return;
  }

  t.launched = std::move(*launched);
  t.launch_pending = true;
  t.launch_deadline = Clock::now() + Milliseconds(std::max(1, t.launch_timeout_ms));
  ++t.launch_count;
  ATLAS_LOG_WARNING("Reviver: launched {} attempt={} pid={} exe={} port={}", t.slug,
                    t.restart_attempts, t.launched.Pid(), exe.string(), port);
}

auto Reviver::PriorityTakeoverGrace(uint8_t priority) -> Duration {
  // Higher priority → shorter grace: the top priority takes over instantly, a
  // lower one defers so a more-preferred survivor goes first. Registry dedup is
  // the correctness backstop; the grace just avoids redundant launch attempts.
  return std::chrono::duration_cast<Duration>(
      std::chrono::milliseconds(static_cast<int>(255 - priority) * 20));
}

auto Reviver::ShouldSupervise(const ManagedTarget& t) const -> bool {
  // Subject-designated active monitor, or the one it last designated and the
  // subject just died → supervise immediately.
  if (t.is_active_reviver) return true;
  if (t.active) {
    // Subject alive. Stand by only once it has actually designated someone else
    // (we received an ack and it said we are not active). Before the first ack
    // — e.g. right after we launched it — the sole/launching Reviver supervises
    // provisionally; AuditRevive is a no-op while the subject is up, so this
    // never double-launches against a higher-priority peer.
    return t.heartbeat_last_ack_at == TimePoint{};
  }
  // Subject down and we were never designated: take over once our priority-
  // scaled grace elapses (highest-priority survivor goes first).
  if (t.subject_down_at == TimePoint{}) return false;
  return Clock::now() - t.subject_down_at >= PriorityTakeoverGrace(t.priority);
}

auto Reviver::MatchesTargetName(const ManagedTarget& t, std::string_view name) const -> bool {
  return t.configured_name.empty() || name == t.configured_name;
}

auto Reviver::CanCheckLocalPid() const -> bool {
  // Liveness probes only make sense for processes the Reviver itself
  // launched OR processes on loopback (single-host development).
  return true;
}

auto Reviver::TargetStatus(const ManagedTarget& t) const -> std::string {
  // restart_limit_reached only happens to a Reviver that was supervising and
  // gave up, so report it before the standby check (a standby never restarts).
  if (t.restart_limit_reached) return "restart_limited";
  if (!ShouldSupervise(t)) return "standby";
  if (t.active) return "active";
  if (t.launch_pending) return "launching";
  if (t.restart_timer.IsValid()) return "restart_scheduled";
  if (t.query_pending) return "querying";
  return "idle";
}

auto Reviver::HeartbeatLastAckAgeMsForWatcher(const ManagedTarget& t) const -> int64_t {
  return AgeMsSince(t.heartbeat_last_ack_at);
}

}  // namespace atlas
