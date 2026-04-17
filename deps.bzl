"""Atlas third-party dependencies not available in the Bazel Central Registry.

These are declared as http_archive and exposed via a module extension so that
they integrate with Bzlmod (MODULE.bazel).
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("//tools/dotnet:dotnet_host_repo.bzl", "dotnet_host_repo")

def _atlas_deps_impl(module_ctx):
    # rapidjson — header-only JSON library
    # Pinned to same commit as CMake FetchContent (ab1842a)
    http_archive(
        name = "rapidjson",
        url = "https://github.com/Tencent/rapidjson/archive/ab1842a2dae061284c0a62dca1cc6d5e7e37e346.tar.gz",
        strip_prefix = "rapidjson-ab1842a2dae061284c0a62dca1cc6d5e7e37e346",
        build_file = "//third_party:rapidjson.BUILD",
    )

    # zlib 1.3.1
    http_archive(
        name = "zlib",
        url = "https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz",
        strip_prefix = "zlib-1.3.1",
        sha256 = "17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c",
        build_file = "//third_party:zlib.BUILD",
    )

    # sqlite3 3.47.2 amalgamation
    http_archive(
        name = "sqlite3",
        url = "https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip",
        strip_prefix = "sqlite-amalgamation-3470200",
        build_file = "//third_party:sqlite3.BUILD",
    )

    # .NET host (hostfxr / nethost) — system dependency
    # Auto-detects installed .NET SDK via DOTNET_ROOT or standard paths.
    dotnet_host_repo(
        name = "dotnet_host",
    )

atlas_deps = module_extension(
    implementation = _atlas_deps_impl,
)
