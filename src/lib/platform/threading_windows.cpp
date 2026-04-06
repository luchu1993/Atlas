#include "platform/threading.hpp"

#if ATLAS_PLATFORM_WINDOWS

// clang-format off
#include <windows.h>         // Must precede all other Windows SDK headers
#include <processthreadsapi.h>
// clang-format on

namespace atlas
{

void set_thread_name(std::string_view name)
{
    std::wstring wname(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), wname.c_str());
}

void set_thread_name(std::jthread& thread, std::string_view name)
{
    std::wstring wname(name.begin(), name.end());
    SetThreadDescription(thread.native_handle(), wname.c_str());
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_WINDOWS
