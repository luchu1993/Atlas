#include "platform/io_poller.hpp"

#if ATLAS_PLATFORM_WINDOWS

#include <format>
#include <limits>
#include <unordered_map>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace atlas
{

class WSAPollPoller final : public IOPoller
{
public:
    auto add(FdHandle fd, IOEvent interest, IOCallback callback) -> Result<void> override
    {
        if (entries_.count(fd) != 0)
        {
            return Error(ErrorCode::AlreadyExists, "fd already registered with poller");
        }

        WSAPOLLFD pfd{};
        pfd.fd = static_cast<SOCKET>(fd);
        pfd.events = to_wsa_events(interest);

        pollfds_.push_back(pfd);
        entries_[fd] = Entry{interest, std::move(callback), pollfds_.size() - 1};
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

        it->second.interest = interest;
        pollfds_[it->second.index].events = to_wsa_events(interest);
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

        const auto idx = it->second.index;
        entries_.erase(it);

        if (idx < pollfds_.size() - 1)
        {
            pollfds_[idx] = pollfds_.back();
            const auto swapped_fd = static_cast<FdHandle>(pollfds_[idx].fd);
            auto swapped_it = entries_.find(swapped_fd);
            if (swapped_it != entries_.end())
            {
                swapped_it->second.index = idx;
            }
        }
        pollfds_.pop_back();
        ++generation_;
        return Result<void>{};
    }

    auto poll(Duration max_wait) -> Result<int> override
    {
        if (pollfds_.empty())
        {
            return 0;
        }

        auto ms = std::chrono::duration_cast<Milliseconds>(max_wait).count();
        int timeout_ms;
        if (ms <= 0)
        {
            timeout_ms = 0;
        }
        else if (ms > static_cast<decltype(ms)>(std::numeric_limits<int>::max()))
        {
            timeout_ms = -1;
        }
        else
        {
            timeout_ms = static_cast<int>(ms);
        }

        const int result =
            ::WSAPoll(pollfds_.data(), static_cast<ULONG>(pollfds_.size()), timeout_ms);
        if (result == SOCKET_ERROR)
        {
            return Error(ErrorCode::IoError,
                         std::format("WSAPoll() failed: {}", ::WSAGetLastError()));
        }
        if (result == 0)
        {
            return 0;
        }

        ready_fds_.clear();
        for (auto& pfd : pollfds_)
        {
            if (pfd.revents != 0)
            {
                ready_fds_.push_back({static_cast<FdHandle>(pfd.fd), from_wsa_events(pfd.revents)});
                pfd.revents = 0;
            }
        }

        int dispatched = 0;
        for (auto [fd, events] : ready_fds_)
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
        std::size_t index;
    };

    struct ReadyFd
    {
        FdHandle fd;
        IOEvent events;
    };

    static auto to_wsa_events(IOEvent interest) -> SHORT
    {
        SHORT events = 0;
        if ((interest & IOEvent::Readable) != IOEvent::None)
        {
            events |= POLLRDNORM;
        }
        if ((interest & IOEvent::Writable) != IOEvent::None)
        {
            events |= POLLWRNORM;
        }
        return events;
    }

    static auto from_wsa_events(SHORT revents) -> IOEvent
    {
        IOEvent result = IOEvent::None;
        if ((revents & (POLLRDNORM | POLLIN)) != 0)
        {
            result |= IOEvent::Readable;
        }
        if ((revents & (POLLWRNORM | POLLOUT)) != 0)
        {
            result |= IOEvent::Writable;
        }
        if ((revents & POLLERR) != 0)
        {
            result |= IOEvent::Error;
        }
        if ((revents & POLLHUP) != 0)
        {
            result |= IOEvent::HangUp;
        }
        return result;
    }

    uint64_t generation_{0};
    std::unordered_map<FdHandle, Entry> entries_;
    std::vector<WSAPOLLFD> pollfds_;
    std::vector<ReadyFd> ready_fds_;
};

std::unique_ptr<IOPoller> create_wsapoll_poller()
{
    return std::make_unique<WSAPollPoller>();
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_WINDOWS
