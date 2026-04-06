#include "platform/io_poller.hpp"

namespace atlas
{

// Forward declarations for platform-specific factory functions
std::unique_ptr<IOPoller> create_select_poller();

#if ATLAS_PLATFORM_LINUX
std::unique_ptr<IOPoller> create_epoll_poller();
#endif

auto IOPoller::create() -> std::unique_ptr<IOPoller>
{
#if ATLAS_PLATFORM_LINUX
    return create_epoll_poller();
#else
    return create_select_poller();
#endif
}

}  // namespace atlas
