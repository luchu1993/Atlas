#include "clrscript/clr_host.h"
#include "foundation/log.h"

#ifdef __linux__

#include <climits>
#include <format>

#include <hostfxr.h>
#include <nethost.h>

namespace atlas {

auto ClrHost::LoadHostfxr() -> Result<void> {
  // On Linux char_t = char; get_hostfxr_path returns a UTF-8 path.
  char buffer[PATH_MAX];
  size_t buffer_size = PATH_MAX;

  int rc = get_hostfxr_path(buffer, &buffer_size, nullptr);
  if (rc != 0) {
    return Error{ErrorCode::kScriptError, std::format("get_hostfxr_path failed: 0x{:08X}. "
                                                      "Is a .NET runtime installed?",
                                                      static_cast<unsigned>(rc))};
  }

  auto lib_result = DynamicLibrary::Load(std::filesystem::path(buffer));
  if (!lib_result) {
    return Error{ErrorCode::kScriptError,
                 std::format("Failed to load hostfxr: {}", lib_result.Error().Message())};
  }
  hostfxr_lib_ = std::move(*lib_result);

  auto sym_init = hostfxr_lib_->GetSymbol<hostfxr_initialize_for_runtime_config_fn>(
      "hostfxr_initialize_for_runtime_config");
  auto sym_delegate =
      hostfxr_lib_->GetSymbol<hostfxr_get_runtime_delegate_fn>("hostfxr_get_runtime_delegate");
  auto sym_close = hostfxr_lib_->GetSymbol<hostfxr_close_fn>("hostfxr_close");

  if (!sym_init || !sym_delegate || !sym_close) {
    hostfxr_lib_.reset();
    return Error{ErrorCode::kScriptError, "Failed to resolve required hostfxr symbols"};
  }

  fn_init_config_ = reinterpret_cast<void*>(*sym_init);
  fn_get_delegate_ = reinterpret_cast<void*>(*sym_delegate);
  fn_close_ = reinterpret_cast<void*>(*sym_close);

  ATLAS_LOG_DEBUG("ClrHost: hostfxr loaded from {}", buffer);
  return {};
}

}  // namespace atlas

#endif  // __linux__
