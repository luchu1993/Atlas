#include "clrscript/clr_bootstrap.hpp"

#include "foundation/error.hpp"
#include "foundation/log.hpp"

#include <format>

namespace atlas
{

namespace
{

using BootstrapFn = int (*)(ClrBootstrapArgs*, ClrObjectVTableOut*);

// Shared implementation.
auto clr_bootstrap_impl(ClrHost& host, const std::filesystem::path& runtime_dll,
                        ClrBootstrapArgs args) -> Result<void>
{
    auto method = host.get_method_as<BootstrapFn>(
        runtime_dll, "Atlas.Core.Bootstrap, Atlas.Runtime", "Initialize");
    if (!method)
        return Error{ErrorCode::ScriptError,
                     std::format("clr_bootstrap: failed to resolve Bootstrap.Initialize: {}",
                                 method.error().message())};

    ClrObjectVTableOut vtable_out{};

    const int rc = (*method)(&args, &vtable_out);
    if (rc != 0)
        return Error{ErrorCode::ScriptError,
                     "clr_bootstrap: Bootstrap.Initialize() returned non-zero — check stderr"};

    if (!vtable_out.free_handle || !vtable_out.get_type_name || !vtable_out.is_none ||
        !vtable_out.to_int64 || !vtable_out.to_double || !vtable_out.to_string ||
        !vtable_out.to_bool)
        return Error{ErrorCode::ScriptError,
                     "clr_bootstrap: Bootstrap.Initialize() returned incomplete vtable"};

    ClrObjectVTable vtable{};
    vtable.free_handle = vtable_out.free_handle;
    vtable.get_type_name = vtable_out.get_type_name;
    vtable.is_none = vtable_out.is_none;
    vtable.to_int64 = vtable_out.to_int64;
    vtable.to_double = vtable_out.to_double;
    vtable.to_string = vtable_out.to_string;
    vtable.to_bool = vtable_out.to_bool;

    set_clr_object_vtable(vtable);

    ATLAS_LOG_INFO("CLR bootstrap complete — ErrorBridge and ClrObjectVTable registered");
    return {};
}

}  // namespace

// Default: uses the local module's clr_error_* functions (production path).
auto clr_bootstrap(ClrHost& host, const std::filesystem::path& runtime_dll) -> Result<void>
{
    return clr_bootstrap_impl(host, runtime_dll, ClrBootstrapArgs{});
}

// Custom args: caller supplies DLL-sourced error function pointers.
auto clr_bootstrap(ClrHost& host, const std::filesystem::path& runtime_dll, ClrBootstrapArgs args)
    -> Result<void>
{
    return clr_bootstrap_impl(host, runtime_dll, args);
}

}  // namespace atlas
