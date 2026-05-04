#include "child_process.h"
#include "platform/platform_config.h"

#if ATLAS_PLATFORM_LINUX

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace atlas {

struct ChildProcess::Impl {
  pid_t pid{-1};
  int stdout_fd{-1};  // read end in parent
  std::thread reader;

  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> lines;
  std::string pending;
  bool reader_eof{false};

  std::atomic<bool> reaped{false};
  int cached_exit{0};
};

ChildProcess::ChildProcess() = default;
ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;
ChildProcess& ChildProcess::operator=(ChildProcess&&) noexcept = default;

ChildProcess::~ChildProcess() {
  if (!impl_) return;
  if (impl_->pid > 0 && !impl_->reaped.load()) {
    kill(impl_->pid, SIGTERM);
    // Give the child a short grace period then SIGKILL.
    for (int i = 0; i < 20; ++i) {
      int status = 0;
      pid_t r = waitpid(impl_->pid, &status, WNOHANG);
      if (r == impl_->pid) {
        impl_->reaped.store(true);
        break;
      }
      ::usleep(100 * 1000);
    }
    if (!impl_->reaped.load()) {
      kill(impl_->pid, SIGKILL);
      int status = 0;
      waitpid(impl_->pid, &status, 0);
      impl_->reaped.store(true);
    }
  }
  if (impl_->stdout_fd >= 0) {
    close(impl_->stdout_fd);
    impl_->stdout_fd = -1;
  }
  if (impl_->reader.joinable()) impl_->reader.join();
}

auto ChildProcess::Start(Options opts) -> Result<ChildProcess> {
  if (opts.exe.empty()) {
    return Error{ErrorCode::kInvalidArgument, "ChildProcess::Start: exe path empty"};
  }

  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) {
    return Error{ErrorCode::kInternalError, std::string("pipe() failed: ") + strerror(errno)};
  }

  // Self-pipe + FD_CLOEXEC: a successful exec closes the write end (parent
  // reads EOF), a failure writes errno through it for synchronous reporting.
  int errpipe[2] = {-1, -1};
  if (pipe(errpipe) != 0) {
    int err = errno;
    close(pipefd[0]);
    close(pipefd[1]);
    return Error{ErrorCode::kInternalError, std::string("pipe() failed: ") + strerror(err)};
  }
  if (fcntl(errpipe[1], F_SETFD, FD_CLOEXEC) != 0) {
    int err = errno;
    close(pipefd[0]);
    close(pipefd[1]);
    close(errpipe[0]);
    close(errpipe[1]);
    return Error{ErrorCode::kInternalError,
                 std::string("fcntl(FD_CLOEXEC) failed: ") + strerror(err)};
  }

  pid_t pid = fork();
  if (pid < 0) {
    int err = errno;
    close(pipefd[0]);
    close(pipefd[1]);
    close(errpipe[0]);
    close(errpipe[1]);
    return Error{ErrorCode::kInternalError, std::string("fork() failed: ") + strerror(err)};
  }

  if (pid == 0) {
    close(pipefd[0]);
    close(errpipe[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    auto report_and_exit = [&](int err) {
      ssize_t n = ::write(errpipe[1], &err, sizeof(err));
      (void)n;
      _exit(127);
    };

    if (!opts.working_directory.empty()) {
      if (chdir(opts.working_directory.c_str()) != 0) report_and_exit(errno);
    }

    std::vector<std::string> storage;
    storage.reserve(1 + opts.args.size());
    storage.emplace_back(opts.exe.string());
    for (auto& a : opts.args) storage.emplace_back(a);

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(storage.size() + 1);
    for (auto& s : storage) argv_ptrs.push_back(s.data());
    argv_ptrs.push_back(nullptr);

    execvp(argv_ptrs[0], argv_ptrs.data());
    report_and_exit(errno);
  }

  close(pipefd[1]);
  close(errpipe[1]);

  int child_errno = 0;
  ssize_t er = 0;
  while (true) {
    er = ::read(errpipe[0], &child_errno, sizeof(child_errno));
    if (er >= 0 || errno != EINTR) break;
  }
  close(errpipe[0]);

  if (er > 0) {
    // Reap so the failed exec doesn't leave a zombie behind.
    int status = 0;
    waitpid(pid, &status, 0);
    close(pipefd[0]);
    return Error{ErrorCode::kInternalError,
                 std::string("execvp(") + opts.exe.string() + ") failed: " + strerror(child_errno)};
  }

  ChildProcess cp;
  cp.impl_ = std::make_unique<Impl>();
  cp.impl_->pid = pid;
  cp.impl_->stdout_fd = pipefd[0];

  Impl* impl = cp.impl_.get();
  cp.impl_->reader = std::thread([impl] {
    char buf[4096];
    while (true) {
      ssize_t n = ::read(impl->stdout_fd, buf, sizeof(buf));
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) break;  // EOF

      std::lock_guard<std::mutex> lock(impl->mu);
      for (ssize_t i = 0; i < n; ++i) {
        char c = buf[i];
        if (c == '\n') {
          if (!impl->pending.empty() && impl->pending.back() == '\r') {
            impl->pending.pop_back();
          }
          impl->lines.emplace_back(std::move(impl->pending));
          impl->pending.clear();
        } else {
          impl->pending.push_back(c);
        }
      }
      impl->cv.notify_all();
    }
    std::lock_guard<std::mutex> lock(impl->mu);
    if (!impl->pending.empty()) {
      impl->lines.emplace_back(std::move(impl->pending));
      impl->pending.clear();
    }
    impl->reader_eof = true;
    impl->cv.notify_all();
  });

  return cp;
}

auto ChildProcess::TryReadStdoutLine() -> std::optional<std::string> {
  if (!impl_) return std::nullopt;
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->lines.empty()) return std::nullopt;
  auto s = std::move(impl_->lines.front());
  impl_->lines.pop_front();
  return s;
}

auto ChildProcess::WaitForStdoutLine(std::chrono::milliseconds timeout)
    -> std::optional<std::string> {
  if (!impl_) return std::nullopt;
  std::unique_lock<std::mutex> lock(impl_->mu);
  if (!impl_->cv.wait_for(lock, timeout,
                          [this] { return !impl_->lines.empty() || impl_->reader_eof; })) {
    return std::nullopt;
  }
  if (impl_->lines.empty()) return std::nullopt;
  auto s = std::move(impl_->lines.front());
  impl_->lines.pop_front();
  return s;
}

auto ChildProcess::IsRunning() const -> bool {
  if (!impl_ || impl_->pid <= 0) return false;
  if (impl_->reaped.load()) return false;
  int status = 0;
  pid_t r = waitpid(impl_->pid, &status, WNOHANG);
  if (r == 0) return true;  // still running
  if (r == impl_->pid) {
    impl_->reaped.store(true);
    if (WIFEXITED(status)) {
      impl_->cached_exit = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      impl_->cached_exit = 128 + WTERMSIG(status);
    }
    return false;
  }
  return false;
}

void ChildProcess::Kill() {
  if (!impl_ || impl_->pid <= 0) return;
  kill(impl_->pid, SIGTERM);
}

auto ChildProcess::Wait(std::chrono::milliseconds timeout) -> std::optional<int> {
  if (!impl_ || impl_->pid <= 0) return std::nullopt;
  if (impl_->reaped.load()) return impl_->cached_exit;
  // Poll-based wait because waitpid has no native timeout. 10 ms granularity is
  // acceptable for a test harness.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    pid_t r = waitpid(impl_->pid, &status, WNOHANG);
    if (r == impl_->pid) {
      impl_->reaped.store(true);
      if (WIFEXITED(status)) {
        impl_->cached_exit = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        impl_->cached_exit = 128 + WTERMSIG(status);
      }
      return impl_->cached_exit;
    }
    ::usleep(10 * 1000);
  }
  return std::nullopt;
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_LINUX
