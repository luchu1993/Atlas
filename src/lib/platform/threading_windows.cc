#include "platform/threading.h"

#if ATLAS_PLATFORM_WINDOWS

// clang-format off
#include <windows.h>         // Must precede all other Windows SDK headers
#include <processthreadsapi.h>
// clang-format on

namespace atlas {

void SetThreadName(std::string_view name) {
  std::wstring wname(name.begin(), name.end());
  SetThreadDescription(GetCurrentThread(), wname.c_str());
}

void SetThreadName(std::jthread& thread, std::string_view name) {
  std::wstring wname(name.begin(), name.end());
  SetThreadDescription(thread.native_handle(), wname.c_str());
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_WINDOWS
