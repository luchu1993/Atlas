#pragma once

#include "foundation/error.hpp"
#include "platform/dynamic_library.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace atlas
{

// ============================================================================
// ClrHost — CoreCLR runtime host via hostfxr
// ============================================================================
//
// Embeds the .NET runtime into the C++ process using the official hostfxr API.
// hostfxr SDK headers (nethost.h, hostfxr.h, coreclr_delegates.h) are kept
// out of this public header — only implementation files include them.
//
// Lifecycle:
//   initialize(runtimeconfig) -> get_method() ... -> finalize()
//
// Thread safety:
//   initialize() and finalize() must be called from the main thread.
//   get_method() is safe after initialization.

class ClrHost
{
public:
    ClrHost() = default;
    ~ClrHost();

    ClrHost(const ClrHost&) = delete;
    ClrHost& operator=(const ClrHost&) = delete;
    ClrHost(ClrHost&&) noexcept;
    ClrHost& operator=(ClrHost&&) noexcept;

    // Initialize CoreCLR using the specified runtimeconfig.json.
    [[nodiscard]] auto initialize(const std::filesystem::path& runtime_config_path) -> Result<void>;

    // Shut down CoreCLR and release all resources.
    void finalize();

    // Retrieve a function pointer to a C# [UnmanagedCallersOnly] static method.
    //
    //   assembly_path — full path to the managed .dll
    //   type_name     — assembly-qualified type, e.g.
    //                   "Atlas.Runtime.Bootstrap, Atlas.Runtime"
    //   method_name   — method name, e.g. "Initialize"
    //
    // Cast the returned void* to the appropriate function pointer type before
    // calling. The ClrHost must remain alive for any calls through that pointer.
    [[nodiscard]] auto get_method(const std::filesystem::path& assembly_path,
                                  std::string_view type_name, std::string_view method_name)
        -> Result<void*>;

    [[nodiscard]] auto is_initialized() const -> bool { return initialized_; }

private:
    // Platform-specific: locate and load hostfxr, resolve function pointers.
    // Implemented in clr_host_windows.cpp / clr_host_linux.cpp.
    [[nodiscard]] auto load_hostfxr() -> Result<void>;

    // hostfxr shared library — must outlive all cached function pointers.
    std::optional<DynamicLibrary> hostfxr_lib_;

    // hostfxr context handle (opaque to callers)
    void* host_context_ = nullptr;

    // Resolved hostfxr entry points stored as void* to avoid pulling in
    // hostfxr SDK headers in this public header. Cast to the correct type
    // in clr_host.cpp before use.
    void* fn_init_config_ = nullptr;
    void* fn_get_delegate_ = nullptr;
    void* fn_close_ = nullptr;

    // CoreCLR managed-to-native bridge delegate
    void* fn_load_assembly_ = nullptr;

    bool initialized_ = false;
};

}  // namespace atlas
