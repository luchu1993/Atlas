#include "reviver.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "foundation/log.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "server/machined_client.h"
#include "server/watcher.h"

namespace atlas {

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

  startup_check_at_ = Clock::now() + std::chrono::milliseconds(500);
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kCellAppMgr,
      [this](const machined::BirthNotification& msg) { OnCellAppMgrBirth(msg); },
      [this](const machined::DeathNotification& msg) { OnCellAppMgrDeath(msg); });
  return true;
}

void Reviver::Fini() {
  if (restart_timer_.IsValid()) {
    (void)Dispatcher().CancelTimer(restart_timer_);
    restart_timer_ = {};
  }
  ManagerApp::Fini();
}

void Reviver::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<bool>("reviver/cellappmgr/active",
               std::function<bool()>([this] { return cellappmgr_active_; }));
  wr.Add<uint32_t>("reviver/cellappmgr/active_pid",
                   std::function<uint32_t()>([this] { return last_cellappmgr_pid_; }));
  wr.Add<uint32_t>("reviver/cellappmgr/launched_pid",
                   std::function<uint32_t()>([this] { return launched_cellappmgr_pid_; }));
  wr.Add<uint32_t>("reviver/cellappmgr/restart_attempts",
                   std::function<uint32_t()>([this] { return restart_attempts_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/launch_count",
                   std::function<uint64_t()>([this] { return launch_count_; }));
  wr.Add<uint64_t>("reviver/cellappmgr/launch_failures",
                   std::function<uint64_t()>([this] { return launch_failures_; }));
  wr.Add<std::string>("reviver/cellappmgr/last_error",
                      std::function<std::string()>([this] { return last_error_; }));
}

void Reviver::OnTickComplete() {
  AuditColdStart();
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
  cellappmgr_active_ = true;
  last_cellappmgr_addr_ = addr;
  last_cellappmgr_pid_ = pid;
  last_error_.clear();
  ATLAS_LOG_INFO("Reviver: CellAppMgr active name={} pid={} addr={}",
                 name, pid, addr.ToString());
}

void Reviver::OnCellAppMgrDeath(const machined::DeathNotification& msg) {
  if (!MatchesTargetName(msg.name)) return;
  cellappmgr_active_ = false;
  last_cellappmgr_addr_ = msg.internal_addr;
  ATLAS_LOG_WARNING("Reviver: CellAppMgr death name={} reason={} addr={}",
                    msg.name, msg.reason, msg.internal_addr.ToString());
  if (msg.reason == 0) return;
  ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
}

void Reviver::AuditColdStart() {
  if (startup_checked_ || Clock::now() < startup_check_at_) return;
  startup_checked_ = true;
  if (!Config().revive_cellappmgr_on_start || cellappmgr_active_) return;
  GetMachinedClient().QueryAsync(ProcessType::kCellAppMgr,
                                 [this](std::vector<machined::ProcessInfo> infos) {
    if (cellappmgr_active_) return;
    for (const auto& info : infos) {
      if (!MatchesTargetName(info.name)) continue;
      RememberCellAppMgr(info.name, info.internal_addr, info.pid);
      return;
    }
    ScheduleCellAppMgrRestart(Duration::zero());
  });
}

void Reviver::ScheduleCellAppMgrRestart(Duration delay) {
  if (restart_timer_.IsValid()) return;
  if (restart_attempts_ >= static_cast<uint32_t>(std::max(0, Config().revive_max_restarts))) {
    last_error_ = "restart limit reached";
    ATLAS_LOG_ERROR("Reviver: CellAppMgr restart limit reached");
    return;
  }
  restart_timer_ = Dispatcher().AddTimer(delay, [this](TimerHandle) {
    restart_timer_ = {};
    LaunchCellAppMgr();
  });
}

void Reviver::LaunchCellAppMgr() {
  if (cellappmgr_active_) return;
  ++restart_attempts_;
  const auto exe = ResolveCellAppMgrExe();
  const uint16_t port = CellAppMgrPortForLaunch();
  if (exe.empty() || !std::filesystem::exists(exe)) {
    ++launch_failures_;
    last_error_ = std::format("CellAppMgr exe not found: {}", exe.string());
    ATLAS_LOG_ERROR("Reviver: {}", last_error_);
    return;
  }
  if (port == 0) {
    ++launch_failures_;
    last_error_ = "CellAppMgr internal port is not configured";
    ATLAS_LOG_ERROR("Reviver: {}", last_error_);
    return;
  }

  std::vector<std::string> args{
      "--type", "cellappmgr",
      "--name", Config().revive_cellappmgr_name,
      "--internal-port", std::to_string(port),
      "--machined", Config().machined_address.ToString(),
      "--update-hertz", std::to_string(Config().revive_cellappmgr_update_hertz)};
  const auto snapshot_path = Config().revive_cellappmgr_snapshot_path.empty()
                                 ? Config().snapshot_path
                                 : Config().revive_cellappmgr_snapshot_path;
  if (!snapshot_path.empty()) {
    args.push_back("--snapshot-path");
    args.push_back(snapshot_path.string());
  }

  ProcessLaunchOptions opts;
  opts.exe = exe;
  opts.args = std::move(args);
  opts.working_directory = exe.parent_path();
  auto pid = LaunchDetachedProcess(std::move(opts));
  if (!pid) {
    ++launch_failures_;
    last_error_ = pid.Error().Message();
    ATLAS_LOG_ERROR("Reviver: failed to launch CellAppMgr: {}", last_error_);
    ScheduleCellAppMgrRestart(Milliseconds(Config().revive_restart_delay_ms));
    return;
  }

  launched_cellappmgr_pid_ = *pid;
  ++launch_count_;
  ATLAS_LOG_WARNING("Reviver: launched CellAppMgr attempt={} pid={} exe={} port={}",
                    restart_attempts_, launched_cellappmgr_pid_, exe.string(), port);
}

auto Reviver::MatchesTargetName(std::string_view name) const -> bool {
  const auto& target = Config().revive_cellappmgr_name;
  return target.empty() || name == target;
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

}  // namespace atlas
