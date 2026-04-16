#include "clrscript/clr_host.h"

#include "foundation/log.h"

// hostfxr SDK headers — included only in implementation files, not the public header
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
    : hostfxr_lib(std::move(other.hostfxr_lib)),
      host_context(other.host_context),
      fn_init_config(other.fn_init_config),
      fn_get_delegate(other.fn_get_delegate),
      fn_close(other.fn_close),
      fn_load_assembly(other.fn_load_assembly),
      initialized_(other.initialized_) {
  other.host_context = nullptr;
  other.fn_init_config = nullptr;
  other.fn_get_delegate = nullptr;
  other.fn_close = nullptr;
  other.fn_load_assembly = nullptr;
  other.initialized_ = false;
}

ClrHost& ClrHost::operator=(ClrHost&& other) noexcept {
  if (this != &other) {
    if (initialized_) Finalize();

    hostfxr_lib = std::move(other.hostfxr_lib);
    host_context = other.host_context;
    fn_init_config = other.fn_init_config;
    fn_get_delegate = other.fn_get_delegate;
    fn_close = other.fn_close;
    fn_load_assembly = other.fn_load_assembly;
    initialized_ = other.initialized_;

    other.host_context = nullptr;
    other.fn_init_config = nullptr;
    other.fn_get_delegate = nullptr;
    other.fn_close = nullptr;
    other.fn_load_assembly = nullptr;
    other.initialized_ = false;
  }
  return *this;
}

auto ClrHost::Initialize(const std::filesystem::path& runtime_config_path) -> Result<void> {
  if (initialized_) return Error{ErrorCode::kScriptError, "ClrHost already initialized"};

  // Step 1: Load hostfxr and resolve its entry points (platform-specific)
  if (auto r = LoadHostfxr(); !r) return r;

// Step 2: Initialize CoreCLR with the supplied runtimeconfig.json
// char_t = wchar_t on Windows, char on Linux/macOS
#if ATLAS_PLATFORM_WINDOWS
  auto config_str = runtime_config_path.wstring();
#else
  auto config_str = runtime_config_path.string();
#endif

  hostfxr_handle ctx = nullptr;
  int rc = AsInitFn(fn_init_config)(config_str.c_str(), nullptr, &ctx);
  if (rc != 0 || ctx == nullptr) {
    return Error{ErrorCode::kScriptError,
                 std::format("hostfxr_initialize_for_runtime_config failed: 0x{:08X}", rc)};
  }
  host_context = ctx;

  // Step 3: Obtain the load_assembly_and_get_function_pointer delegate
  void* load_assembly_fn = nullptr;
  rc = AsDelegateFn(fn_get_delegate)(static_cast<hostfxr_handle>(host_context),
                                     hdt_load_assembly_and_get_function_pointer, &load_assembly_fn);
  if (rc != 0 || load_assembly_fn == nullptr) {
    AsCloseFn(fn_close)(static_cast<hostfxr_handle>(host_context));
    host_context = nullptr;
    return Error{ErrorCode::kScriptError,
                 std::format("hostfxr_get_runtime_delegate failed: 0x{:08X}", rc)};
  }
  fn_load_assembly = load_assembly_fn;

  initialized_ = true;
  ATLAS_LOG_INFO("ClrHost: CoreCLR initialized (config: {})", runtime_config_path.string());
  return {};
}

void ClrHost::Finalize() {
  if (!initialized_) return;

  fn_load_assembly = nullptr;

  if (host_context) {
    // as_close_fn() still valid here: fn_close is nulled below, after the call.
    AsCloseFn(fn_close)(static_cast<hostfxr_handle>(host_context));
    host_context = nullptr;
  }

  // Null all function pointers that point into the shared library BEFORE
  // unloading it, eliminating any window of dangling function pointers.
  fn_init_config = nullptr;
  fn_get_delegate = nullptr;
  fn_close = nullptr;

  // hostfxr_lib destructor unloads the shared library — must happen after
  // all function pointers referencing library symbols are cleared.
  hostfxr_lib.reset();

  initialized_ = false;

  ATLAS_LOG_INFO("ClrHost: CoreCLR finalized");
}

auto ClrHost::GetMethod(const std::filesystem::path& assembly_path, std::string_view type_name,
                        std::string_view method_name) -> Result<void*> {
  if (!initialized_) return Error{ErrorCode::kScriptError, "ClrHost not initialized"};

  void* method_ptr = nullptr;

#if ATLAS_PLATFORM_WINDOWS
  // char_t = wchar_t on Windows
  auto w_assembly = assembly_path.wstring();
  auto w_type = Utf8ToWide(type_name);
  auto w_method = Utf8ToWide(method_name);

  int rc = AsLoadFn(fn_load_assembly)(w_assembly.c_str(), w_type.c_str(), w_method.c_str(),
                                      UNMANAGEDCALLERSONLY_METHOD, nullptr, &method_ptr);
#else
  // char_t = char on Linux/macOS
  auto s_assembly = assembly_path.string();
  auto s_type = std::string(type_name);
  auto s_method = std::string(method_name);

  int rc = AsLoadFn(fn_load_assembly)(s_assembly.c_str(), s_type.c_str(), s_method.c_str(),
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
