#include "clrscript/clr_host.h"
#include "foundation/log.h"

#if ATLAS_PLATFORM_WINDOWS

#include <hostfxr.h>
#include <nethost.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <format>

namespace atlas {

auto ClrHost::LoadHostfxr() -> Result<void> {
  // Use get_hostfxr_path() from nethost to locate hostfxr.dll portably.
  // On Windows char_t = wchar_t.
  wchar_t buffer[MAX_PATH];
  size_t buffer_size = MAX_PATH;

  int rc = get_hostfxr_path(buffer, &buffer_size, nullptr);
  if (rc != 0) {
    return Error{ErrorCode::kScriptError, std::format("get_hostfxr_path failed: 0x{:08X}. "
                                                      "Is a .NET runtime installed?",
                                                      static_cast<unsigned>(rc))};
  }

  // Load hostfxr.dll — stored as a member so the library stays loaded.
  auto lib_result = DynamicLibrary::Load(std::filesystem::path(buffer));
  if (!lib_result) {
    return Error{ErrorCode::kScriptError,
                 std::format("Failed to load hostfxr: {}", lib_result.Error().Message())};
  }
  hostfxr_lib_ = std::move(*lib_result);

  // Resolve the three hostfxr entry points we need.
  auto sym_init = hostfxr_lib_->GetSymbol<hostfxr_initialize_for_runtime_config_fn>(
      "hostfxr_initialize_for_runtime_config");
  auto sym_delegate =
      hostfxr_lib_->GetSymbol<hostfxr_get_runtime_delegate_fn>("hostfxr_get_runtime_delegate");
  auto sym_close = hostfxr_lib_->GetSymbol<hostfxr_close_fn>("hostfxr_close");

  if (!sym_init || !sym_delegate || !sym_close) {
    hostfxr_lib_.reset();
    return Error{ErrorCode::kScriptError, "Failed to resolve required hostfxr symbols"};
  }

  // Store as void* — cast back to typed pointers in clr_host.cpp via macros
  fn_init_config_ = reinterpret_cast<void*>(*sym_init);
  fn_get_delegate_ = reinterpret_cast<void*>(*sym_delegate);
  fn_close_ = reinterpret_cast<void*>(*sym_close);

  ATLAS_LOG_DEBUG("ClrHost: hostfxr loaded from {}", std::filesystem::path(buffer).string());
  return {};
}

}  // namespace atlas

#endif  // _WIN32
