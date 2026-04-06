#include "platform/io_poller.hpp"

#if ATLAS_PLATFORM_LINUX

#include <unordered_map>
#include <sys/epoll.h>
#include <unistd.h>

namespace atlas
{

// ============================================================================
// EpollPoller
// ============================================================================

class EpollPoller final : public IOPoller
{
public:
    EpollPoller()
        : epfd_(::epoll_create1(EPOLL_CLOEXEC))
    {
    }

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
            return Error(ErrorCode::IoError, "epoll_wait() failed");
        }

        // Dispatch callbacks — a callback may call add/remove/modify,
        // so we must re-lookup after each callback invocation.
        int dispatched = 0;

        for (int i = 0; i < result; ++i)
        {
            FdHandle fd = static_cast<FdHandle>(events[i].data.fd);

            // Re-lookup each time — previous callback may have removed this fd
            auto it = entries_.find(fd);
            if (it == entries_.end())
            {
                continue;
            }

            // Copy callback before invoking (callback may erase this entry)
            auto callback = it->second.callback;
            IOEvent io_events = from_epoll_events(events[i].events);
            callback(fd, io_events);
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
    std::unordered_map<FdHandle, Entry> entries_;
};

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<IOPoller> create_epoll_poller()
{
    return std::make_unique<EpollPoller>();
}

} // namespace atlas

#endif // ATLAS_PLATFORM_LINUX
