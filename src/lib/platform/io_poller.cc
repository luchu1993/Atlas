#include "platform/io_poller.h"

namespace atlas {

#if ATLAS_PLATFORM_WINDOWS
std::unique_ptr<IOPoller> CreateWsapollPoller();
#else
std::unique_ptr<IOPoller> CreateSelectPoller();
#endif

#if ATLAS_PLATFORM_LINUX
std::unique_ptr<IOPoller> CreateEpollPoller();
#if defined(ATLAS_HAS_IO_URING)
std::unique_ptr<IOPoller> CreateIoUringPoller();
#endif
#endif

auto IOPoller::Create() -> std::unique_ptr<IOPoller> {
#if ATLAS_PLATFORM_LINUX
#if defined(ATLAS_HAS_IO_URING)
  if (auto uring = CreateIoUringPoller()) {
    return uring;
  }
#endif
  return CreateEpollPoller();
#elif ATLAS_PLATFORM_WINDOWS
  return CreateWsapollPoller();
#else
  return CreateSelectPoller();
#endif
}

}  // namespace atlas
