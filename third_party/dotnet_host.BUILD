# .NET host (hostfxr / nethost) — system dependency
# Uses the dynamic nethost (nethost.dll) to avoid /MT vs /MD CRT mismatch.

cc_import(
    name = "dotnet_nethost_import",
    interface_library = "nethost.lib",
    shared_library = "nethost.dll",
)

cc_library(
    name = "dotnet_nethost",
    hdrs = [
        "coreclr_delegates.h",
        "hostfxr.h",
        "nethost.h",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":dotnet_nethost_import"],
)
