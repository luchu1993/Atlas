#include <unordered_map>

#include "platform/io_poller.h"

#if ATLAS_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/select.h>
#include <sys/time.h>

#include <cerrno>
#include <cstring>
#endif

namespace atlas {

class SelectPoller final : public IOPoller {
 public:
  auto Add(FdHandle fd, IOEvent interest, IOCallback callback) -> Result<void> override {
#if !ATLAS_PLATFORM_WINDOWS
    if (static_cast<int>(fd) >= FD_SETSIZE) {
      return Error(ErrorCode::kOutOfRange,
                   std::format("fd {} exceeds FD_SETSIZE ({})", fd, FD_SETSIZE));
    }
#endif
    if (entries_.count(fd) != 0) {
      return Error(ErrorCode::kAlreadyExists, "fd already registered with poller");
    }

    entries_[fd] = Entry{interest, std::move(callback)};
    ++generation_;
    return Result<void>{};
  }

  auto Modify(FdHandle fd, IOEvent interest) -> Result<void> override {
    auto it = entries_.find(fd);
    if (it == entries_.end()) {
      return Error(ErrorCode::kNotFound, "fd not registered with poller");
    }

    it->second.interest = interest;
    ++generation_;
    return Result<void>{};
  }

  auto Remove(FdHandle fd) -> Result<void> override {
    auto it = entries_.find(fd);
    if (it == entries_.end()) {
      return Error(ErrorCode::kNotFound, "fd not registered with poller");
    }

    entries_.erase(it);
    ++generation_;
    return Result<void>{};
  }

  auto Poll(Duration max_wait) -> Result<int> override {
    if (entries_.empty()) {
      return 0;
    }

    fd_set read_set;
    fd_set write_set;
    fd_set except_set;

    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&except_set);

#if !ATLAS_PLATFORM_WINDOWS
    int max_fd = -1;
#endif

    for (const auto& [fd, entry] : entries_) {
      if ((entry.interest & IOEvent::kReadable) != IOEvent::kNone) {
        FD_SET(fd, &read_set);
      }
      if ((entry.interest & IOEvent::kWritable) != IOEvent::kNone) {
        FD_SET(fd, &write_set);
      }
      FD_SET(fd, &except_set);

#if !ATLAS_PLATFORM_WINDOWS
      if (static_cast<int>(fd) > max_fd) {
        max_fd = static_cast<int>(fd);
      }
#endif
    }

    auto usec = std::chrono::duration_cast<Microseconds>(max_wait).count();
    if (usec < 0) {
      usec = 0;
    }

    struct timeval tv;
    // tv_usec is int on Darwin, long on Linux — cast through decltype.
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(usec / 1'000'000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>(usec % 1'000'000);

#if ATLAS_PLATFORM_WINDOWS
    // Winsock select ignores the nfds parameter
    int result = ::select(0, &read_set, &write_set, &except_set, &tv);
#else
    int result = ::select(max_fd + 1, &read_set, &write_set, &except_set, &tv);
#endif

    if (result < 0) {
#if ATLAS_PLATFORM_WINDOWS
      int err = ::WSAGetLastError();
      return Error(ErrorCode::kIoError, std::format("select() failed: WSA error {}", err));
#else
      if (errno == EINTR) {
        return 0;
      }
      return Error(ErrorCode::kIoError,
                   std::format("select() failed: {} (errno={})", std::strerror(errno), errno));
#endif
    }

    if (result == 0) {
      return 0;
    }

    ready_fds_.clear();
    for (const auto& [fd, entry] : entries_) {
      IOEvent events = IOEvent::kNone;
      if (FD_ISSET(fd, &read_set)) events |= IOEvent::kReadable;
      if (FD_ISSET(fd, &write_set)) events |= IOEvent::kWritable;
      if (FD_ISSET(fd, &except_set)) events |= IOEvent::kError;
      if (events != IOEvent::kNone) ready_fds_.push_back({fd, events});
    }

    int dispatched = 0;
    for (auto [fd, events] : ready_fds_) {
      auto it = entries_.find(fd);
      if (it == entries_.end()) {
        continue;
      }

      const auto gen_before = generation_;
      IOCallback cb;
      std::swap(it->second.callback, cb);
      cb(fd, events);
      if (generation_ != gen_before) {
        it = entries_.find(fd);
      }
      if (it != entries_.end() && !it->second.callback) {
        std::swap(it->second.callback, cb);
      }
      ++dispatched;
    }

    return dispatched;
  }

 private:
  struct Entry {
    IOEvent interest;
    IOCallback callback;
  };

  struct ReadyFd {
    FdHandle fd;
    IOEvent events;
  };

  uint64_t generation_{0};
  std::unordered_map<FdHandle, Entry> entries_;
  std::vector<ReadyFd> ready_fds_;
};

std::unique_ptr<IOPoller> CreateSelectPoller() {
  return std::make_unique<SelectPoller>();
}

}  // namespace atlas
