#include "platform/threading.h"

#if ATLAS_PLATFORM_LINUX

#include <pthread.h>

#include <algorithm>
#include <cstddef>

namespace atlas {

void SetThreadName(std::string_view name) {
  // pthread_setname_np is limited to 16 chars including null terminator
  char buf[16];
  auto len = std::min(name.size(), static_cast<std::size_t>(15));
  std::copy_n(name.data(), len, buf);
  buf[len] = '\0';
  pthread_setname_np(pthread_self(), buf);
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_LINUX
