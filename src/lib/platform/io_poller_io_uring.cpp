#include "platform/io_poller.hpp"

#if ATLAS_PLATFORM_LINUX && defined(ATLAS_HAS_IO_URING)

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <liburing.h>
#include <limits>
#include <poll.h>
#include <unordered_map>
#include <vector>

namespace atlas
{

namespace
{

constexpr auto poll_user_data(FdHandle fd) -> uint64_t
{
    return static_cast<uint64_t>(fd) + 1u;
}

constexpr auto cancel_completion_user_data() -> uint64_t
{
    return UINT64_MAX;
}

}  // namespace

class IoUringPoller final : public IOPoller
{
public:
    static constexpr unsigned kQueueDepth = 4096;

    IoUringPoller()
    {
        if (::io_uring_queue_init(kQueueDepth, &ring_, 0) == 0)
        {
            valid_ = true;
        }
    }

    ~IoUringPoller() override
    {
        if (valid_)
        {
            ::io_uring_queue_exit(&ring_);
        }
    }

    IoUringPoller(const IoUringPoller&) = delete;
    auto operator=(const IoUringPoller&) -> IoUringPoller& = delete;
    IoUringPoller(IoUringPoller&&) = delete;
    auto operator=(IoUringPoller&&) -> IoUringPoller& = delete;

    [[nodiscard]] auto is_valid() const -> bool { return valid_; }

    auto add(FdHandle fd, IOEvent interest, IOCallback callback) -> Result<void> override
    {
        if (!valid_)
        {
            return Error(ErrorCode::InternalError, "io_uring not initialized");
        }
        if (entries_.count(fd) != 0)
        {
            return Error(ErrorCode::AlreadyExists, "fd already registered");
        }

        entries_[fd] = Entry{interest, std::move(callback)};
        if (!submit_poll(fd, interest))
        {
            entries_.erase(fd);
            return Error(ErrorCode::IoError, "io_uring poll submission failed");
        }
        ++generation_;
        return Result<void>{};
    }

    auto modify(FdHandle fd, IOEvent interest) -> Result<void> override
    {
        auto it = entries_.find(fd);
        if (it == entries_.end())
        {
            return Error(ErrorCode::NotFound, "fd not registered");
        }

        const IOEvent previous = it->second.interest;
        if (!cancel_poll(fd))
        {
            return Error(ErrorCode::IoError, "io_uring cancel submission failed");
        }
        it->second.interest = interest;
        if (!submit_poll(fd, interest))
        {
            it->second.interest = previous;
            (void)submit_poll(fd, previous);
            return Error(ErrorCode::IoError, "io_uring poll submission failed");
        }
        ++generation_;
        return Result<void>{};
    }

    auto remove(FdHandle fd) -> Result<void> override
    {
        auto it = entries_.find(fd);
        if (it == entries_.end())
        {
            return Error(ErrorCode::NotFound, "fd not registered");
        }

        if (!cancel_poll(fd))
        {
            return Error(ErrorCode::IoError, "io_uring cancel submission failed");
        }
        entries_.erase(it);
        ++generation_;
        return Result<void>{};
    }

    auto poll(Duration max_wait) -> Result<int> override
    {
        if (!valid_)
        {
            return Error(ErrorCode::InternalError, "io_uring not initialized");
        }

        (void)::io_uring_submit(&ring_);

        auto ms = std::chrono::duration_cast<Milliseconds>(max_wait).count();

        if (ms > 0)
        {
            struct io_uring_cqe* wait_cqe = nullptr;
            if (ms > static_cast<decltype(ms)>(std::numeric_limits<int>::max()))
            {
                if (::io_uring_wait_cqe(&ring_, &wait_cqe) < 0)
                {
                    return Error(ErrorCode::IoError, "io_uring_wait_cqe() failed");
                }
            }
            else
            {
                struct __kernel_timespec ts{};
                const auto total_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Milliseconds{ms}).count();
                ts.tv_sec = total_ns / 1'000'000'000LL;
                ts.tv_nsec = static_cast<long>(total_ns % 1'000'000'000LL);
                const int wr = ::io_uring_wait_cqe_timeout(&ring_, &wait_cqe, &ts, 0);
                if (wr < 0 && wr != -ETIME)
                {
                    return Error(ErrorCode::IoError, "io_uring_wait_cqe_timeout() failed");
                }
            }
            (void)wait_cqe;
        }

        unsigned head;
        unsigned count = 0;
        static constexpr unsigned kMaxCqesBatch = 256;
        struct io_uring_cqe* cqes[kMaxCqesBatch]{};
        struct io_uring_cqe* cqe = nullptr;
        ::io_uring_for_each_cqe(&ring_, head, cqe)
        {
            cqes[count] = cqe;
            ++count;
            if (count >= kMaxCqesBatch)
            {
                break;
            }
        }

        struct ReadyFd
        {
            FdHandle fd;
            IOEvent events;
        };

        // First pass: collect ready fds and advance CQ head in batch
        std::vector<ReadyFd> ready_fds;
        for (unsigned i = 0; i < count; ++i)
        {
            cqe = cqes[i];
            const uint64_t ud = cqe->user_data;
            if (ud == cancel_completion_user_data())
            {
                continue;
            }

            const auto fd = static_cast<FdHandle>(static_cast<int>(ud - 1u));

            IOEvent events = IOEvent::None;
            if (cqe->res < 0)
            {
                events = IOEvent::Error;
            }
            else
            {
                const int revents = cqe->res;
                if ((revents & POLLIN) != 0)
                {
                    events |= IOEvent::Readable;
                }
                if ((revents & POLLOUT) != 0)
                {
                    events |= IOEvent::Writable;
                }
                if ((revents & POLLERR) != 0)
                {
                    events |= IOEvent::Error;
                }
                if ((revents & POLLHUP) != 0)
                {
                    events |= IOEvent::HangUp;
                }
            }
            ready_fds.push_back({fd, events});
        }

        ::io_uring_cq_advance(&ring_, count);

        int dispatched = 0;
        for (auto [fd, events] : ready_fds)
        {
            auto it = entries_.find(fd);
            if (it == entries_.end())
            {
                continue;
            }

            const auto gen_before = generation_;
            IOCallback cb;
            std::swap(it->second.callback, cb);
            cb(fd, events);
            if (generation_ != gen_before)
            {
                it = entries_.find(fd);
            }
            if (it != entries_.end() && !it->second.callback)
            {
                std::swap(it->second.callback, cb);
            }
            ++dispatched;
        }

        return dispatched;
    }

private:
    struct Entry
    {
        IOEvent interest;
        IOCallback callback;
    };

    auto acquire_sqe() -> ::io_uring_sqe*
    {
        ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            (void)::io_uring_submit(&ring_);
            sqe = ::io_uring_get_sqe(&ring_);
        }
        return sqe;
    }

    auto submit_poll(FdHandle fd, IOEvent interest) -> bool
    {
        ::io_uring_sqe* sqe = acquire_sqe();
        if (sqe == nullptr)
        {
            return false;
        }

        unsigned poll_mask = 0;
        if ((interest & IOEvent::Readable) != IOEvent::None)
        {
            poll_mask |= POLLIN;
        }
        if ((interest & IOEvent::Writable) != IOEvent::None)
        {
            poll_mask |= POLLOUT;
        }

        ::io_uring_prep_poll_add(sqe, fd, poll_mask);
        sqe->len = IORING_POLL_ADD_MULTI;
        ::io_uring_sqe_set_data64(sqe, poll_user_data(fd));
        return true;
    }

    auto cancel_poll(FdHandle fd) -> bool
    {
        ::io_uring_sqe* sqe = acquire_sqe();
        if (sqe == nullptr)
        {
            return false;
        }
        ::io_uring_prep_cancel64(sqe, poll_user_data(fd), 0);
        ::io_uring_sqe_set_data64(sqe, cancel_completion_user_data());
        return true;
    }

    bool valid_{false};
    struct io_uring ring_{};
    uint64_t generation_{0};
    std::unordered_map<FdHandle, Entry> entries_;
};

auto create_io_uring_poller() -> std::unique_ptr<IOPoller>
{
    auto poller = std::make_unique<IoUringPoller>();
    if (!poller->is_valid())
    {
        return nullptr;
    }
    return std::unique_ptr<IOPoller>(poller.release());
}

}  // namespace atlas

#endif
