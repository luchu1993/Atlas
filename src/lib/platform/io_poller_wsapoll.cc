#include "platform/io_poller.h"

#if ATLAS_PLATFORM_WINDOWS

#include <winsock2.h>
#include <ws2tcpip.h>

#include <format>
#include <limits>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace atlas {

class WSAPollPoller final : public IOPoller {
 public:
  auto Add(FdHandle fd, IOEvent interest, IOCallback callback) -> Result<void> override {
    if (entries_.count(fd) != 0) {
      return Error(ErrorCode::kAlreadyExists, "fd already registered with poller");
    }

    WSAPOLLFD pfd{};
    pfd.fd = static_cast<SOCKET>(fd);
    pfd.events = ToWsaEvents(interest);

    pollfds_.push_back(pfd);
    entries_[fd] = Entry{interest, std::move(callback), pollfds_.size() - 1};
    ++generation_;
    return Result<void>{};
  }

  auto Modify(FdHandle fd, IOEvent interest) -> Result<void> override {
    auto it = entries_.find(fd);
    if (it == entries_.end()) {
      return Error(ErrorCode::kNotFound, "fd not registered with poller");
    }

    it->second.interest = interest;
    pollfds_[it->second.index].events = ToWsaEvents(interest);
    ++generation_;
    return Result<void>{};
  }

  auto Remove(FdHandle fd) -> Result<void> override {
    auto it = entries_.find(fd);
    if (it == entries_.end()) {
      return Error(ErrorCode::kNotFound, "fd not registered with poller");
    }

    const auto kIdx = it->second.index;
    entries_.erase(it);

    if (kIdx < pollfds_.size() - 1) {
      pollfds_[kIdx] = pollfds_.back();
      const auto kSwappedFd = static_cast<FdHandle>(pollfds_[kIdx].fd);
      auto swapped_it = entries_.find(kSwappedFd);
      if (swapped_it != entries_.end()) {
        swapped_it->second.index = kIdx;
      }
    }
    pollfds_.pop_back();
    ++generation_;
    return Result<void>{};
  }

  auto Poll(Duration max_wait) -> Result<int> override {
    if (pollfds_.empty()) {
      return 0;
    }

    auto ms = std::chrono::duration_cast<Milliseconds>(max_wait).count();
    int timeout_ms;
    if (ms <= 0) {
      timeout_ms = 0;
    } else if (ms > static_cast<decltype(ms)>((std::numeric_limits<int>::max)())) {
      timeout_ms = -1;
    } else {
      timeout_ms = static_cast<int>(ms);
    }

    const int kResult = ::WSAPoll(pollfds_.data(), static_cast<ULONG>(pollfds_.size()), timeout_ms);
    if (kResult == SOCKET_ERROR) {
      return Error(ErrorCode::kIoError, std::format("WSAPoll() failed: {}", ::WSAGetLastError()));
    }
    if (kResult == 0) {
      return 0;
    }

    ready_fds_.clear();
    for (auto& pfd : pollfds_) {
      if (pfd.revents != 0) {
        ready_fds_.push_back({static_cast<FdHandle>(pfd.fd), FromWsaEvents(pfd.revents)});
        pfd.revents = 0;
      }
    }

    int dispatched = 0;
    for (auto [fd, events] : ready_fds_) {
      auto it = entries_.find(fd);
      if (it == entries_.end()) {
        continue;
      }

      const auto kGenBefore = generation_;
      IOCallback cb;
      std::swap(it->second.callback, cb);
      cb(fd, events);
      if (generation_ != kGenBefore) {
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
    std::size_t index;
  };

  struct ReadyFd {
    FdHandle fd;
    IOEvent events;
  };

  static auto ToWsaEvents(IOEvent interest) -> SHORT {
    SHORT events = 0;
    if ((interest & IOEvent::kReadable) != IOEvent::kNone) {
      events |= POLLRDNORM;
    }
    if ((interest & IOEvent::kWritable) != IOEvent::kNone) {
      events |= POLLWRNORM;
    }
    return events;
  }

  static auto FromWsaEvents(SHORT revents) -> IOEvent {
    IOEvent result = IOEvent::kNone;
    if ((revents & (POLLRDNORM | POLLIN)) != 0) {
      result |= IOEvent::kReadable;
    }
    if ((revents & (POLLWRNORM | POLLOUT)) != 0) {
      result |= IOEvent::kWritable;
    }
    if ((revents & POLLERR) != 0) {
      result |= IOEvent::kError;
    }
    if ((revents & POLLHUP) != 0) {
      result |= IOEvent::kHangUp;
    }
    return result;
  }

  uint64_t generation_{0};
  std::unordered_map<FdHandle, Entry> entries_;
  std::vector<WSAPOLLFD> pollfds_;
  std::vector<ReadyFd> ready_fds_;
};

std::unique_ptr<IOPoller> CreateWsapollPoller() {
  return std::make_unique<WSAPollPoller>();
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_WINDOWS
