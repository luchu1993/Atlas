#include "process_launcher.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace atlas {
namespace {

// /proc/$pid/stat field 22 (1-indexed): process start time in clock
// ticks since boot. Read at launch to bind LaunchedProcess identity to
// THIS incarnation of pid — a recycled pid will show a different value.
// Returns 0 on parse failure (treat as "no identity guard").
auto ReadStartTicks(pid_t pid) -> uint64_t {
  std::array<char, 64> path_buf{};
  std::snprintf(path_buf.data(), path_buf.size(), "/proc/%d/stat", static_cast<int>(pid));
  std::ifstream stat(path_buf.data());
  if (!stat.is_open()) return 0;
  std::string line;
  if (!std::getline(stat, line)) return 0;
  // comm field is in parens and may contain spaces — skip past final ')'
  // then split the remainder on spaces. start_time is index 19 from there
  // (field 22 - the pid, comm, state fields we already consumed).
  const auto rparen = line.rfind(')');
  if (rparen == std::string::npos || rparen + 2 >= line.size()) return 0;
  std::string_view tail(line.data() + rparen + 2, line.size() - rparen - 2);
  for (int field = 0; field < 19; ++field) {
    const auto sp = tail.find(' ');
    if (sp == std::string_view::npos) return 0;
    tail.remove_prefix(sp + 1);
  }
  const auto sp = tail.find(' ');
  std::string_view ticks_view = sp == std::string_view::npos ? tail : tail.substr(0, sp);
  uint64_t ticks = 0;
  for (char c : ticks_view) {
    if (c < '0' || c > '9') return 0;
    ticks = ticks * 10 + static_cast<uint64_t>(c - '0');
  }
  return ticks;
}

}  // namespace

LaunchedProcess::LaunchedProcess(LaunchedProcess&& other) noexcept
    : pid_(other.pid_), platform_token_(other.platform_token_) {
  other.pid_ = 0;
  other.platform_token_ = 0;
}

auto LaunchedProcess::operator=(LaunchedProcess&& other) noexcept -> LaunchedProcess& {
  if (this != &other) {
    pid_ = other.pid_;
    platform_token_ = other.platform_token_;
    other.pid_ = 0;
    other.platform_token_ = 0;
  }
  return *this;
}

LaunchedProcess::~LaunchedProcess() { Reset(); }

void LaunchedProcess::Reset() {
  pid_ = 0;
  platform_token_ = 0;
}

auto LaunchedProcess::IsAlive() const -> bool {
  if (pid_ == 0) return false;
  const auto ticks_now = ReadStartTicks(static_cast<pid_t>(pid_));
  if (ticks_now == 0) {
    // /proc/$pid/stat gone — process either exited or never existed.
    if (kill(static_cast<pid_t>(pid_), 0) != 0) return false;
    return errno == EPERM;  // EPERM means a process exists; we just can't read
  }
  if (platform_token_ != 0 && ticks_now != platform_token_) return false;  // pid reused
  return true;
}

auto LaunchedProcess::Terminate() -> bool {
  if (!IsAlive()) return false;
  return kill(static_cast<pid_t>(pid_), SIGKILL) == 0 || errno == ESRCH;
}

auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<LaunchedProcess> {
  if (opts.exe.empty()) {
    return Error{ErrorCode::kInvalidArgument, "LaunchDetachedProcess: exe path empty"};
  }

  int output_fd = -1;
  if (!opts.output_path.empty()) {
    const auto parent = opts.output_path.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        return Error{ErrorCode::kInternalError,
                     "create output directory failed: " + ec.message()};
      }
    }
    output_fd = open(opts.output_path.string().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (output_fd < 0) {
      return Error{ErrorCode::kInternalError,
                   "open output failed: " + std::string(std::strerror(errno))};
    }
  }

  int pipe_fds[2]{};
  if (pipe(pipe_fds) != 0) {
    if (output_fd >= 0) close(output_fd);
    return Error{ErrorCode::kInternalError,
                 "pipe failed: " + std::string(std::strerror(errno))};
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    if (output_fd >= 0) close(output_fd);
    return Error{ErrorCode::kInternalError,
                 "fork failed: " + std::string(std::strerror(errno))};
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    (void)setsid();
    const pid_t child_pid = fork();
    if (child_pid < 0) {
      const pid_t error_pid = -1;
      (void)write(pipe_fds[1], &error_pid, sizeof(error_pid));
      if (output_fd >= 0) close(output_fd);
      _exit(127);
    }
    if (child_pid > 0) {
      (void)write(pipe_fds[1], &child_pid, sizeof(child_pid));
      if (output_fd >= 0) close(output_fd);
      _exit(0);
    }
    close(pipe_fds[1]);
    if (output_fd >= 0) {
      if (dup2(output_fd, STDOUT_FILENO) < 0 || dup2(output_fd, STDERR_FILENO) < 0) {
        _exit(127);
      }
      if (output_fd > STDERR_FILENO) close(output_fd);
    }
    if (!opts.working_directory.empty()) {
      (void)chdir(opts.working_directory.string().c_str());
    }

    std::vector<std::string> storage;
    storage.reserve(opts.args.size() + 1);
    storage.push_back(opts.exe.string());
    for (const auto& arg : opts.args) storage.push_back(arg);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    execvp(storage[0].c_str(), argv.data());
    _exit(127);
  }

  close(pipe_fds[1]);
  if (output_fd >= 0) close(output_fd);
  pid_t launched_pid = -1;
  auto* out = reinterpret_cast<char*>(&launched_pid);
  std::size_t total = 0;
  while (total < sizeof(launched_pid)) {
    const ssize_t bytes = read(pipe_fds[0], out + total, sizeof(launched_pid) - total);
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes <= 0) break;
    total += static_cast<std::size_t>(bytes);
  }
  close(pipe_fds[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (total != sizeof(launched_pid) || launched_pid <= 0) {
    return Error{ErrorCode::kInternalError, "detached launch helper failed"};
  }

  const auto start_ticks = ReadStartTicks(launched_pid);
  return LaunchedProcess(static_cast<uint32_t>(launched_pid), start_ticks);
}

auto IsProcessAlive(uint32_t pid) -> bool {
  if (pid == 0) return false;
  if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
}

auto TerminateProcessByPid(uint32_t pid) -> bool {
  if (pid == 0) return false;
  if (kill(static_cast<pid_t>(pid), SIGKILL) == 0) return true;
  return errno == ESRCH;
}

}  // namespace atlas
