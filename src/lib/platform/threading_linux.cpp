#include "platform/threading.hpp"

#if ATLAS_PLATFORM_LINUX

#include <algorithm>
#include <cstddef>
#include <pthread.h>

namespace atlas
{

void set_thread_name(std::string_view name)
{
    // pthread_setname_np is limited to 16 chars including null terminator
    char buf[16];
    auto len = std::min(name.size(), static_cast<std::size_t>(15));
    std::copy_n(name.data(), len, buf);
    buf[len] = '\0';
    pthread_setname_np(pthread_self(), buf);
}

void set_thread_name(std::jthread& thread, std::string_view name)
{
    char buf[16];
    auto len = std::min(name.size(), static_cast<std::size_t>(15));
    std::copy_n(name.data(), len, buf);
    buf[len] = '\0';
    pthread_setname_np(thread.native_handle(), buf);
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_LINUX
