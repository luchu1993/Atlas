#include "platform/io_poller.hpp"

namespace atlas
{

#if ATLAS_PLATFORM_WINDOWS
std::unique_ptr<IOPoller> create_wsapoll_poller();
#else
std::unique_ptr<IOPoller> create_select_poller();
#endif

#if ATLAS_PLATFORM_LINUX
std::unique_ptr<IOPoller> create_epoll_poller();
#if defined(ATLAS_HAS_IO_URING)
std::unique_ptr<IOPoller> create_io_uring_poller();
#endif
#endif

auto IOPoller::create() -> std::unique_ptr<IOPoller>
{
#if ATLAS_PLATFORM_LINUX
#if defined(ATLAS_HAS_IO_URING)
    if (auto uring = create_io_uring_poller())
    {
        return uring;
    }
#endif
    return create_epoll_poller();
#elif ATLAS_PLATFORM_WINDOWS
    return create_wsapoll_poller();
#else
    return create_select_poller();
#endif
}

}  // namespace atlas
