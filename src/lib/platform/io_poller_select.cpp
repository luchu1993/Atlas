#include "platform/io_poller.hpp"

#include <unordered_map>

#if ATLAS_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace atlas
{

// ============================================================================
// SelectPoller
// ============================================================================

class SelectPoller final : public IOPoller
{
public:
    auto add(FdHandle fd, IOEvent interest, IOCallback callback) -> Result<void> override
    {
#if !ATLAS_PLATFORM_WINDOWS
        if (static_cast<int>(fd) >= FD_SETSIZE)
        {
            return Error(ErrorCode::OutOfRange,
                         std::format("fd {} exceeds FD_SETSIZE ({})", fd, FD_SETSIZE));
        }
#endif
        if (entries_.count(fd) != 0)
        {
            return Error(ErrorCode::AlreadyExists, "fd already registered with poller");
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

        entries_.erase(it);
        return Result<void>{};
    }

    auto poll(Duration max_wait) -> Result<int> override
    {
        if (entries_.empty())
        {
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

        for (const auto& [fd, entry] : entries_)
        {
            if ((entry.interest & IOEvent::Readable) != IOEvent::None)
            {
                FD_SET(fd, &read_set);
            }
            if ((entry.interest & IOEvent::Writable) != IOEvent::None)
            {
                FD_SET(fd, &write_set);
            }
            // Always monitor for errors
            FD_SET(fd, &except_set);

#if !ATLAS_PLATFORM_WINDOWS
            if (static_cast<int>(fd) > max_fd)
            {
                max_fd = static_cast<int>(fd);
            }
#endif
        }

        // Convert Duration to timeval
        auto usec = std::chrono::duration_cast<Microseconds>(max_wait).count();
        if (usec < 0)
        {
            usec = 0;
        }

        struct timeval tv;
        tv.tv_sec = static_cast<long>(usec / 1'000'000);
        tv.tv_usec = static_cast<long>(usec % 1'000'000);

#if ATLAS_PLATFORM_WINDOWS
        // Winsock select ignores the nfds parameter
        int result = ::select(0, &read_set, &write_set, &except_set, &tv);
#else
        int result = ::select(max_fd + 1, &read_set, &write_set, &except_set, &tv);
#endif

        if (result < 0)
        {
#if ATLAS_PLATFORM_WINDOWS
            int err = ::WSAGetLastError();
            return Error(ErrorCode::IoError, std::format("select() failed: WSA error {}", err));
#else
            return Error(ErrorCode::IoError, std::format("select() failed: {} (errno={})",
                                                         std::strerror(errno), errno));
#endif
        }

        if (result == 0)
        {
            return 0;
        }

        // Collect only the ready {fd, events} pairs into a lightweight snapshot
        // (O(ready) pairs, not O(n) full map copy with std::function heap allocs).
        // ready_fds_ is a member to reuse its buffer across poll() calls.
        ready_fds_.clear();
        for (const auto& [fd, entry] : entries_)
        {
            IOEvent events = IOEvent::None;
            if (FD_ISSET(fd, &read_set))
                events |= IOEvent::Readable;
            if (FD_ISSET(fd, &write_set))
                events |= IOEvent::Writable;
            if (FD_ISSET(fd, &except_set))
                events |= IOEvent::Error;
            if (events != IOEvent::None)
                ready_fds_.push_back({fd, events});
        }

        int dispatched = 0;
        for (auto [fd, events] : ready_fds_)
        {
            // Re-lookup in case a prior callback called remove() on this fd.
            // Copy the callback before invoking — callback may call remove()
            // which destroys the Entry that owns the std::function.
            auto current_it = entries_.find(fd);
            if (current_it != entries_.end())
            {
                auto cb = current_it->second.callback;
                cb(fd, events);
                ++dispatched;
            }
        }

        return dispatched;
    }

private:
    struct Entry
    {
        IOEvent interest;
        IOCallback callback;
    };

    // Reused across poll() calls to avoid per-call heap allocation.
    struct ReadyFd
    {
        FdHandle fd;
        IOEvent events;
    };

    std::unordered_map<FdHandle, Entry> entries_;
    std::vector<ReadyFd> ready_fds_;
};

// ============================================================================
// Factory
// ============================================================================

std::unique_ptr<IOPoller> create_select_poller()
{
    return std::make_unique<SelectPoller>();
}

}  // namespace atlas
