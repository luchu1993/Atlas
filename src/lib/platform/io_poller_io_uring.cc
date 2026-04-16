#include "platform/io_poller.h"

#if ATLAS_PLATFORM_LINUX && defined(ATLAS_HAS_IO_URING)

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include <liburing.h>

namespace atlas {

namespace {

constexpr auto PollUserData(FdHandle fd) -> uint64_t {
  return static_cast<uint64_t>(fd) + 1u;
}

constexpr auto CancelCompletionUserData() -> uint64_t {
  return UINT64_MAX;
}

}  // namespace

class IoUringPoller final : public IOPoller {
 public:
  static constexpr unsigned kQueueDepth = 4096;

  IoUringPoller() {
    if (::io_uring_queue_init(kQueueDepth, &ring_, 0) == 0) {
      valid_ = true;
    }
  }

  ~IoUringPoller() override {
    if (valid_) {
      ::io_uring_queue_exit(&ring_);
    }
  }

  IoUringPoller(const IoUringPoller&) = delete;
  auto operator=(const IoUringPoller&) -> IoUringPoller& = delete;
  IoUringPoller(IoUringPoller&&) = delete;
  auto operator=(IoUringPoller&&) -> IoUringPoller& = delete;

  [[nodiscard]] auto IsValid() const -> bool { return valid_; }

  auto Add(FdHandle fd, IOEvent interest, IOCallback callback) -> Result<void> override {
    if (!valid_) {
      return Error(ErrorCode::kInternalError, "io_uring not initialized");
    }
    if (entries_.count(fd) != 0) {
      return Error(ErrorCode::kAlreadyExists, "fd already registered");
    }

    entries_[fd] = Entry{interest, std::move(callback)};
    if (!SubmitPoll(fd, interest)) {
      entries_.erase(fd);
      return Error(ErrorCode::kIoError, "io_uring poll submission failed");
    }
    ++generation_;
    return Result<void>{};
  }

  auto Modify(FdHandle fd, IOEvent interest) -> Result<void> override {
    auto it = entries_.find(fd);
    if (it == entries_.end()) {
      return Error(ErrorCode::kNotFound, "fd not registered");
    }

    const IOEvent previous = it->second.interest;
    if (!CancelPoll(fd)) {
      return Error(ErrorCode::kIoError, "io_uring cancel submission failed");
    }
    it->second.interest = interest;
    if (!SubmitPoll(fd, interest)) {
      it->second.interest = previous;
      (void)SubmitPoll(fd, previous);
      return Error(ErrorCode::kIoError, "io_uring poll submission failed");
    }
    ++generation_;
    return Result<void>{};
  }

  auto Remove(FdHandle fd) -> Result<void> override {
    auto it = entries_.find(fd);
    if (it == entries_.end()) {
      return Error(ErrorCode::kNotFound, "fd not registered");
    }

    if (!CancelPoll(fd)) {
      return Error(ErrorCode::kIoError, "io_uring cancel submission failed");
    }
    entries_.erase(it);
    ++generation_;
    return Result<void>{};
  }

  auto Poll(Duration max_wait) -> Result<int> override {
    if (!valid_) {
      return Error(ErrorCode::kInternalError, "io_uring not initialized");
    }

    (void)::io_uring_submit(&ring_);

    auto ms = std::chrono::duration_cast<Milliseconds>(max_wait).count();

    if (ms > 0) {
      struct io_uring_cqe* wait_cqe = nullptr;
      if (ms > static_cast<decltype(ms)>(std::numeric_limits<int>::max())) {
        if (::io_uring_wait_cqe(&ring_, &wait_cqe) < 0) {
          return Error(ErrorCode::kIoError, "io_uring_wait_cqe() failed");
        }
      } else {
        struct __kernel_timespec ts{};
        const auto total_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Milliseconds{ms}).count();
        ts.tv_sec = total_ns / 1'000'000'000LL;
        ts.tv_nsec = static_cast<long>(total_ns % 1'000'000'000LL);
        const int wr = ::io_uring_wait_cqe_timeout(&ring_, &wait_cqe, &ts, 0);
        if (wr < 0 && wr != -ETIME) {
          return Error(ErrorCode::kIoError, "io_uring_wait_cqe_timeout() failed");
        }
      }
      (void)wait_cqe;
    }

    unsigned head;
    unsigned count = 0;
    static constexpr unsigned kMaxCqesBatch = 256;
    struct io_uring_cqe* cqes[kMaxCqesBatch]{};
    struct io_uring_cqe* cqe = nullptr;
    ::io_uring_for_each_cqe(&ring_, head, cqe) {
      cqes[count] = cqe;
      ++count;
      if (count >= kMaxCqesBatch) {
        break;
      }
    }

    struct ReadyFd {
      FdHandle fd;
      IOEvent events;
    };

    // First pass: collect ready fds and advance CQ head in batch
    std::vector<ReadyFd> ready_fds;
    for (unsigned i = 0; i < count; ++i) {
      cqe = cqes[i];
      const uint64_t ud = cqe->user_data;
      if (ud == CancelCompletionUserData()) {
        continue;
      }

      const auto fd = static_cast<FdHandle>(static_cast<int>(ud - 1u));

      IOEvent events = IOEvent::kNone;
      if (cqe->res < 0) {
        events = IOEvent::kError;
      } else {
        const int revents = cqe->res;
        if ((revents & POLLIN) != 0) {
          events |= IOEvent::kReadable;
        }
        if ((revents & POLLOUT) != 0) {
          events |= IOEvent::kWritable;
        }
        if ((revents & POLLERR) != 0) {
          events |= IOEvent::kError;
        }
        if ((revents & POLLHUP) != 0) {
          events |= IOEvent::kHangUp;
        }
      }
      ready_fds.push_back({fd, events});
    }

    ::io_uring_cq_advance(&ring_, count);

    int dispatched = 0;
    for (auto [fd, events] : ready_fds) {
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

  auto AcquireSqe() -> ::io_uring_sqe* {
    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
      (void)::io_uring_submit(&ring_);
      sqe = ::io_uring_get_sqe(&ring_);
    }
    return sqe;
  }

  auto SubmitPoll(FdHandle fd, IOEvent interest) -> bool {
    ::io_uring_sqe* sqe = AcquireSqe();
    if (sqe == nullptr) {
      return false;
    }

    unsigned poll_mask = 0;
    if ((interest & IOEvent::kReadable) != IOEvent::kNone) {
      poll_mask |= POLLIN;
    }
    if ((interest & IOEvent::kWritable) != IOEvent::kNone) {
      poll_mask |= POLLOUT;
    }

    ::io_uring_prep_poll_add(sqe, fd, poll_mask);
    sqe->len = IORING_POLL_ADD_MULTI;
    ::io_uring_sqe_set_data64(sqe, PollUserData(fd));
    return true;
  }

  auto CancelPoll(FdHandle fd) -> bool {
    ::io_uring_sqe* sqe = AcquireSqe();
    if (sqe == nullptr) {
      return false;
    }
    ::io_uring_prep_cancel64(sqe, PollUserData(fd), 0);
    ::io_uring_sqe_set_data64(sqe, CancelCompletionUserData());
    return true;
  }

  bool valid_{false};
  struct io_uring ring_{};
  uint64_t generation_{0};
  std::unordered_map<FdHandle, Entry> entries_;
};

auto CreateIoUringPoller() -> std::unique_ptr<IOPoller> {
  auto poller = std::make_unique<IoUringPoller>();
  if (!poller->IsValid()) {
    return nullptr;
  }
  return std::unique_ptr<IOPoller>(poller.release());
}

}  // namespace atlas

#endif
