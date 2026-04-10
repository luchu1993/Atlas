#include "clrscript/clr_host.hpp"

#include "foundation/log.hpp"

// hostfxr SDK headers — included only in implementation files, not the public header
#include <coreclr_delegates.h>
#include <format>
#include <hostfxr.h>

#ifdef _WIN32
#include <windows.h>

namespace
{
// Converts a UTF-8 string_view to a UTF-16 wstring for use with Windows APIs.
// std::wstring(begin, end) widens each byte independently and is incorrect for
// multi-byte UTF-8 sequences (e.g. CJK characters in type/method names).
auto utf8_to_wide(std::string_view utf8) -> std::wstring
{
    if (utf8.empty())
        return {};

    int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return {};

    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(),
                          needed);
    return result;
}
}  // namespace
#endif

namespace atlas
{

// Convenience casts from the void* members to the typed hostfxr function pointers
#define FN_INIT_CONFIG reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(fn_init_config_)
#define FN_GET_DELEGATE reinterpret_cast<hostfxr_get_runtime_delegate_fn>(fn_get_delegate_)
#define FN_CLOSE reinterpret_cast<hostfxr_close_fn>(fn_close_)
#define FN_LOAD_ASSEMBLY \
    reinterpret_cast<load_assembly_and_get_function_pointer_fn>(fn_load_assembly_)

ClrHost::~ClrHost()
{
    if (initialized_)
        finalize();
}

ClrHost::ClrHost(ClrHost&& other) noexcept
    : hostfxr_lib_(std::move(other.hostfxr_lib_)),
      host_context_(other.host_context_),
      fn_init_config_(other.fn_init_config_),
      fn_get_delegate_(other.fn_get_delegate_),
      fn_close_(other.fn_close_),
      fn_load_assembly_(other.fn_load_assembly_),
      initialized_(other.initialized_)
{
    other.host_context_ = nullptr;
    other.fn_init_config_ = nullptr;
    other.fn_get_delegate_ = nullptr;
    other.fn_close_ = nullptr;
    other.fn_load_assembly_ = nullptr;
    other.initialized_ = false;
}

ClrHost& ClrHost::operator=(ClrHost&& other) noexcept
{
    if (this != &other)
    {
        if (initialized_)
            finalize();

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

auto ClrHost::initialize(const std::filesystem::path& runtime_config_path) -> Result<void>
{
    if (initialized_)
        return Error{ErrorCode::ScriptError, "ClrHost already initialized"};

    // Step 1: Load hostfxr and resolve its entry points (platform-specific)
    if (auto r = load_hostfxr(); !r)
        return r;

// Step 2: Initialize CoreCLR with the supplied runtimeconfig.json
// char_t = wchar_t on Windows, char on Linux/macOS
#ifdef _WIN32
    auto config_str = runtime_config_path.wstring();
#else
    auto config_str = runtime_config_path.string();
#endif

    hostfxr_handle ctx = nullptr;
    int rc = FN_INIT_CONFIG(config_str.c_str(), nullptr, &ctx);
    if (rc != 0 || ctx == nullptr)
    {
        return Error{ErrorCode::ScriptError,
                     std::format("hostfxr_initialize_for_runtime_config failed: 0x{:08X}", rc)};
    }
    host_context_ = ctx;

    // Step 3: Obtain the load_assembly_and_get_function_pointer delegate
    void* load_assembly_fn = nullptr;
    rc = FN_GET_DELEGATE(static_cast<hostfxr_handle>(host_context_),
                         hdt_load_assembly_and_get_function_pointer, &load_assembly_fn);
    if (rc != 0 || load_assembly_fn == nullptr)
    {
        FN_CLOSE(static_cast<hostfxr_handle>(host_context_));
        host_context_ = nullptr;
        return Error{ErrorCode::ScriptError,
                     std::format("hostfxr_get_runtime_delegate failed: 0x{:08X}", rc)};
    }
    fn_load_assembly_ = load_assembly_fn;

    initialized_ = true;
    ATLAS_LOG_INFO("ClrHost: CoreCLR initialized (config: {})", runtime_config_path.string());
    return {};
}

void ClrHost::finalize()
{
    if (!initialized_)
        return;

    fn_load_assembly_ = nullptr;

    if (host_context_)
    {
        FN_CLOSE(static_cast<hostfxr_handle>(host_context_));
        host_context_ = nullptr;
    }

    // hostfxr_lib_ destructor unloads the shared library — must happen last.
    hostfxr_lib_.reset();

    fn_init_config_ = nullptr;
    fn_get_delegate_ = nullptr;
    fn_close_ = nullptr;
    initialized_ = false;

    ATLAS_LOG_INFO("ClrHost: CoreCLR finalized");
}

auto ClrHost::get_method(const std::filesystem::path& assembly_path, std::string_view type_name,
                         std::string_view method_name) -> Result<void*>
{
    if (!initialized_)
        return Error{ErrorCode::ScriptError, "ClrHost not initialized"};

    void* method_ptr = nullptr;

#ifdef _WIN32
    // char_t = wchar_t on Windows
    auto w_assembly = assembly_path.wstring();
    auto w_type = utf8_to_wide(type_name);
    auto w_method = utf8_to_wide(method_name);

    int rc = FN_LOAD_ASSEMBLY(w_assembly.c_str(), w_type.c_str(), w_method.c_str(),
                              UNMANAGEDCALLERSONLY_METHOD, nullptr, &method_ptr);
#else
    // char_t = char on Linux/macOS
    auto s_assembly = assembly_path.string();
    auto s_type = std::string(type_name);
    auto s_method = std::string(method_name);

    int rc = FN_LOAD_ASSEMBLY(s_assembly.c_str(), s_type.c_str(), s_method.c_str(),
                              UNMANAGEDCALLERSONLY_METHOD, nullptr, &method_ptr);
#endif

    if (rc != 0 || method_ptr == nullptr)
    {
        return Error{ErrorCode::ScriptError,
                     std::format("Failed to load method '{}.{}' from '{}': 0x{:08X}", type_name,
                                 method_name, assembly_path.string(), rc)};
    }

    return method_ptr;
}

#undef FN_INIT_CONFIG
#undef FN_GET_DELEGATE
#undef FN_CLOSE
#undef FN_LOAD_ASSEMBLY

}  // namespace atlas
