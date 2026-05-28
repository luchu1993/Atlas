#include "process_launcher.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <string>
#include <vector>

namespace atlas {

auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<uint32_t> {
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

  return static_cast<uint32_t>(launched_pid);
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
