#include "process_launcher.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace atlas {

auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<uint32_t> {
  if (opts.exe.empty()) {
    return Error{ErrorCode::kInvalidArgument, "LaunchDetachedProcess: exe path empty"};
  }

  int pipe_fds[2]{};
  if (pipe(pipe_fds) != 0) {
    return Error{ErrorCode::kInternalError,
                 "pipe failed: " + std::string(std::strerror(errno))};
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
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
      _exit(127);
    }
    if (child_pid > 0) {
      (void)write(pipe_fds[1], &child_pid, sizeof(child_pid));
      _exit(0);
    }
    close(pipe_fds[1]);
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

}  // namespace atlas
