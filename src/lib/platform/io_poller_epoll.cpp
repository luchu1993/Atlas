#include "platform/io_poller.hpp"

#if ATLAS_PLATFORM_LINUX

#include <cerrno>
#include <limits>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace atlas
{

// ============================================================================
// EpollPoller
// ============================================================================

class EpollPoller final : public IOPoller
{
public:
    EpollPoller() : epfd_(::epoll_create1(EPOLL_CLOEXEC)) {}

    ~EpollPoller() override
    {
        if (epfd_ >= 0)
        {
            ::close(epfd_);
        }
    }

    // Non-copyable, non-movable
    EpollPoller(const EpollPoller&) = delete;
    auto operator=(const EpollPoller&) -> EpollPoller& = delete;
    EpollPoller(EpollPoller&&) = delete;
    auto operator=(EpollPoller&&) -> EpollPoller& = delete;

    auto add(FdHandle fd, IOEvent interest, IOCallback callback) -> Result<void> override
    {
        if (epfd_ < 0)
        {
            return Error(ErrorCode::InternalError, "epoll instance not initialized");
        }

        if (entries_.count(fd) != 0)
        {
            return Error(ErrorCode::AlreadyExists, "fd already registered with poller");
        }

        struct epoll_event ev = {};
        ev.events = to_epoll_events(interest);
        ev.data.fd = fd;

        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0)
        {
            return Error(ErrorCode::IoError, "epoll_ctl(ADD) failed");
        }

        entries_[fd] = Entry{interest, std::move(callback)};
        ++generation_;
        return Result<void>{};
    }

    auto modify(FdHandle fd, IOEvent interest) -> Result<void> override
    {
        auto it = entries_.find(fd);
        if (it == entries_.end())
        {
            return Error(ErrorCode::NotFound, "fd not registered with poller");
        }

        struct epoll_event ev = {};
        ev.events = to_epoll_events(interest);
        ev.data.fd = fd;

        if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) != 0)
        {
            return Error(ErrorCode::IoError, "epoll_ctl(MOD) failed");
        }

        it->second.interest = interest;
        ++generation_;
        return Result<void>{};
    }

    auto remove(FdHandle fd) -> Result<void> override
    {
        auto it = entries_.find(fd);
        if (it == entries_.end())
        {
            return Error(ErrorCode::NotFound, "fd not registered with poller");
        }

        if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) != 0)
        {
            return Error(ErrorCode::IoError, "epoll_ctl(DEL) failed");
        }

        entries_.erase(it);
        ++generation_;
        return Result<void>{};
    }

    auto poll(Duration max_wait) -> Result<int> override
    {
        if (epfd_ < 0)
        {
            return Error(ErrorCode::InternalError, "epoll instance not initialized");
        }

        // Convert Duration to milliseconds for epoll_wait
        auto ms = std::chrono::duration_cast<Milliseconds>(max_wait).count();
        int timeout_ms;

        if (ms <= 0)
        {
            timeout_ms = 0;
        }
        else if (ms > static_cast<decltype(ms)>(std::numeric_limits<int>::max()))
        {
            timeout_ms = -1;  // Wait indefinitely
        }
        else
        {
            timeout_ms = static_cast<int>(ms);
        }

        struct epoll_event events[kMaxEvents];
        int result = ::epoll_wait(epfd_, events, kMaxEvents, timeout_ms);

        if (result < 0)
        {
            if (errno == EINTR)
            {
                return 0;
            }
            return Error(ErrorCode::IoError, "epoll_wait() failed");
        }

        ready_fds_.clear();
        for (int i = 0; i < result; ++i)
        {
            FdHandle fd = static_cast<FdHandle>(events[i].data.fd);
            IOEvent io_events = from_epoll_events(events[i].events);
            ready_fds_.push_back({fd, io_events});
        }

        int dispatched = 0;
        for (auto [fd, events_flags] : ready_fds_)
        {
            auto it = entries_.find(fd);
            if (it == entries_.end())
            {
                continue;
            }

            const auto gen_before = generation_;
            IOCallback cb;
            std::swap(it->second.callback, cb);
            cb(fd, events_flags);
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
    static constexpr int kMaxEvents = 256;

    struct Entry
    {
        IOEvent interest;
        IOCallback callback;
    };

    struct ReadyFd
    {
        FdHandle fd;
        IOEvent events;
    };

    static auto to_epoll_events(IOEvent interest) -> uint32_t
    {
        uint32_t events = 0;

        if ((interest & IOEvent::Readable) != IOEvent::None)
        {
            events |= EPOLLIN;
        }
        if ((interest & IOEvent::Writable) != IOEvent::None)
        {
            events |= EPOLLOUT;
        }

        return events;
    }

    static auto from_epoll_events(uint32_t events) -> IOEvent
    {
        IOEvent result = IOEvent::None;

        if (events & EPOLLIN)
        {
            result |= IOEvent::Readable;
        }
        if (events & EPOLLOUT)
        {
            result |= IOEvent::Writable;
        }
        if (events & EPOLLERR)
        {
            result |= IOEvent::Error;
        }
        if (events & EPOLLHUP)
        {
            result |= IOEvent::HangUp;
        }

        return result;
    }

    int epfd_{-1};
    uint64_t generation_{0};
    std::unordered_map<FdHandle, Entry> entries_;
    std::vector<ReadyFd> ready_fds_;
};

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<IOPoller> create_epoll_poller()
{
    return std::make_unique<EpollPoller>();
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_LINUX
