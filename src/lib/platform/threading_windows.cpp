#include "platform/threading.hpp"

#if ATLAS_PLATFORM_WINDOWS

#include <processthreadsapi.h>
#include <windows.h>

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
