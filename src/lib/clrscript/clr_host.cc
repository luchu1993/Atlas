#include "clrscript/clr_host.h"

#include "foundation/log.h"

// hostfxr SDK headers stay out of the public header.
#include <format>

#include <coreclr_delegates.h>
#include <hostfxr.h>

#if ATLAS_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace {
#if ATLAS_PLATFORM_WINDOWS
// Converts a UTF-8 string_view to a UTF-16 wstring for use with Windows APIs.
// std::wstring(begin, end) widens each byte independently and is incorrect for
// multi-byte UTF-8 sequences (e.g. CJK characters in type/method names).
auto Utf8ToWide(std::string_view utf8) -> std::wstring {
  if (utf8.empty()) return {};

  int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};

  std::wstring result(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(),
                        needed);
  return result;
}
#endif

// Type-safe casts from void* to the hostfxr function pointer types.
// Inline functions instead of macros: IDE-navigable, type-checked, no #undef.
auto AsInitFn(void* p) -> hostfxr_initialize_for_runtime_config_fn {
  return reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(p);
}
auto AsDelegateFn(void* p) -> hostfxr_get_runtime_delegate_fn {
  return reinterpret_cast<hostfxr_get_runtime_delegate_fn>(p);
}
auto AsCloseFn(void* p) -> hostfxr_close_fn {
  return reinterpret_cast<hostfxr_close_fn>(p);
}
auto AsLoadFn(void* p) -> load_assembly_and_get_function_pointer_fn {
  return reinterpret_cast<load_assembly_and_get_function_pointer_fn>(p);
}
}  // namespace

namespace atlas {

ClrHost::~ClrHost() {
  if (initialized_) Finalize();
}

ClrHost::ClrHost(ClrHost&& other) noexcept
    : hostfxr_lib_(std::move(other.hostfxr_lib_)),
      host_context_(other.host_context_),
      fn_init_config_(other.fn_init_config_),
      fn_get_delegate_(other.fn_get_delegate_),
      fn_close_(other.fn_close_),
      fn_load_assembly_(other.fn_load_assembly_),
      initialized_(other.initialized_) {
  other.host_context_ = nullptr;
  other.fn_init_config_ = nullptr;
  other.fn_get_delegate_ = nullptr;
  other.fn_close_ = nullptr;
  other.fn_load_assembly_ = nullptr;
  other.initialized_ = false;
}

ClrHost& ClrHost::operator=(ClrHost&& other) noexcept {
  if (this != &other) {
    if (initialized_) Finalize();

    hostfxr_lib_ = std::move(other.hostfxr_lib_);
    host_context_ = other.host_context_;
    fn_init_config_ = other.fn_init_config_;
    fn_get_delegate_ = other.fn_get_delegate_;
    fn_close_ = other.fn_close_;
    fn_load_assembly_ = other.fn_load_assembly_;
    initialized_ = other.initialized_;

    other.host_context_ = nullptr;
    other.fn_init_config_ = nullptr;
    other.fn_get_delegate_ = nullptr;
    other.fn_close_ = nullptr;
    other.fn_load_assembly_ = nullptr;
    other.initialized_ = false;
  }
  return *this;
}

auto ClrHost::Initialize(const std::filesystem::path& runtime_config_path) -> Result<void> {
  if (initialized_) return Error{ErrorCode::kScriptError, "ClrHost already initialized"};

  if (auto r = LoadHostfxr(); !r) return r;

#if ATLAS_PLATFORM_WINDOWS
  auto config_str = runtime_config_path.wstring();
#else
  auto config_str = runtime_config_path.string();
#endif

  hostfxr_handle ctx = nullptr;
  int rc = AsInitFn(fn_init_config_)(config_str.c_str(), nullptr, &ctx);
  if (rc != 0 || ctx == nullptr) {
    return Error{ErrorCode::kScriptError,
                 std::format("hostfxr_initialize_for_runtime_config failed: 0x{:08X}", rc)};
  }
  host_context_ = ctx;

  void* load_assembly_fn = nullptr;
  rc =
      AsDelegateFn(fn_get_delegate_)(static_cast<hostfxr_handle>(host_context_),
                                     hdt_load_assembly_and_get_function_pointer, &load_assembly_fn);
  if (rc != 0 || load_assembly_fn == nullptr) {
    AsCloseFn(fn_close_)(static_cast<hostfxr_handle>(host_context_));
    host_context_ = nullptr;
    return Error{ErrorCode::kScriptError,
                 std::format("hostfxr_get_runtime_delegate failed: 0x{:08X}", rc)};
  }
  fn_load_assembly_ = load_assembly_fn;

  initialized_ = true;
  ATLAS_LOG_INFO("ClrHost: CoreCLR initialized (config: {})", runtime_config_path.string());
  return {};
}

void ClrHost::Finalize() {
  if (!initialized_) return;

  fn_load_assembly_ = nullptr;

  if (host_context_) {
    // as_close_fn() still valid here: fn_close_ is nulled below, after the call.
    AsCloseFn(fn_close_)(static_cast<hostfxr_handle>(host_context_));
    host_context_ = nullptr;
  }

  fn_init_config_ = nullptr;
  fn_get_delegate_ = nullptr;
  fn_close_ = nullptr;

  hostfxr_lib_.reset();

  initialized_ = false;

  ATLAS_LOG_INFO("ClrHost: CoreCLR finalized");
}

auto ClrHost::GetMethod(const std::filesystem::path& assembly_path, std::string_view type_name,
                        std::string_view method_name) -> Result<void*> {
  if (!initialized_) return Error{ErrorCode::kScriptError, "ClrHost not initialized"};

  void* method_ptr = nullptr;

#if ATLAS_PLATFORM_WINDOWS
  auto w_assembly = assembly_path.wstring();
  auto w_type = Utf8ToWide(type_name);
  auto w_method = Utf8ToWide(method_name);

  int rc = AsLoadFn(fn_load_assembly_)(w_assembly.c_str(), w_type.c_str(), w_method.c_str(),
                                       UNMANAGEDCALLERSONLY_METHOD, nullptr, &method_ptr);
#else
  // char_t = char on Linux/macOS
  auto s_assembly = assembly_path.string();
  auto s_type = std::string(type_name);
  auto s_method = std::string(method_name);

  int rc = AsLoadFn(fn_load_assembly_)(s_assembly.c_str(), s_type.c_str(), s_method.c_str(),
                                       UNMANAGEDCALLERSONLY_METHOD, nullptr, &method_ptr);
#endif

  if (rc != 0 || method_ptr == nullptr) {
    return Error{ErrorCode::kScriptError,
                 std::format("Failed to load method '{}.{}' from '{}': 0x{:08X}", type_name,
                             method_name, assembly_path.string(), rc)};
  }

  return method_ptr;
}

}  // namespace atlas
