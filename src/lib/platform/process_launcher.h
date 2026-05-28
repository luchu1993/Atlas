#ifndef ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_
#define ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "foundation/error.h"

namespace atlas {

struct ProcessLaunchOptions {
  std::filesystem::path exe;
  std::vector<std::string> args;
  std::filesystem::path working_directory;
  std::filesystem::path output_path;
};

// Owning handle to a process we launched. IsAlive() / Terminate() route
// through the OS handle (Windows) or pid + start_time snapshot (Linux),
// so PID recycling after our child exits cannot fool the supervisor into
// reporting a stranger process as the same one.
class LaunchedProcess {
 public:
  LaunchedProcess() = default;
  LaunchedProcess(const LaunchedProcess&) = delete;
  auto operator=(const LaunchedProcess&) -> LaunchedProcess& = delete;
  LaunchedProcess(LaunchedProcess&& other) noexcept;
  auto operator=(LaunchedProcess&& other) noexcept -> LaunchedProcess&;
  ~LaunchedProcess();

  [[nodiscard]] auto Pid() const -> uint32_t { return pid_; }
  [[nodiscard]] auto IsValid() const -> bool { return pid_ != 0; }
  [[nodiscard]] auto IsAlive() const -> bool;
  auto Terminate() -> bool;
  void Reset();

 private:
  LaunchedProcess(uint32_t pid, uint64_t platform_token) noexcept
      : pid_(pid), platform_token_(platform_token) {}
  friend auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<LaunchedProcess>;

  uint32_t pid_{0};
  // Windows: HANDLE cast to uint64_t; Linux: clock-tick start_time from
  // /proc/$pid/stat (field 22) captured at launch — both serve as the
  // identity guard that survives PID reuse.
  uint64_t platform_token_{0};
};

[[nodiscard]] auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<LaunchedProcess>;

// PID-only liveness probes for processes we did NOT launch (e.g. peers that
// registered with machined). Acknowledged-unsafe under PID reuse — callers
// must accept that a recycled PID can mask process replacement. Prefer
// LaunchedProcess for any process whose lifetime is owned here.
[[nodiscard]] auto IsProcessAlive(uint32_t pid) -> bool;
[[nodiscard]] auto TerminateProcessByPid(uint32_t pid) -> bool;

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_
