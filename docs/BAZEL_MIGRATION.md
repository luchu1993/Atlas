# Bazel Migration Plan

This document describes a complete, step-by-step plan to replace the Atlas Engine CMake build system with Bazel. It is written for the actual project structure found in this repository.

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Why Bazel](#why-bazel)
3. [Migration Strategy](#migration-strategy)
4. [Phase 1 — Toolchain Bootstrap](#phase-1--toolchain-bootstrap)
5. [Phase 2 — Third-Party Dependencies](#phase-2--third-party-dependencies)
6. [Phase 3 — Core Libraries (Bottom-Up)](#phase-3--core-libraries-bottom-up)
7. [Phase 4 — Server Components](#phase-4--server-components)
8. [Phase 5 — Tests](#phase-5--tests)
9. [Phase 6 — C# and CLR Integration](#phase-6--c-and-clr-integration)
10. [Phase 7 — CI and Remote Cache](#phase-7--ci-and-remote-cache)
11. [Phase 8 — CMake Removal](#phase-8--cmake-removal)
12. [Reference: Dependency Graph](#reference-dependency-graph)
13. [Reference: Platform-Specific Files](#reference-platform-specific-files)
14. [Reference: CMake Macro Equivalents](#reference-cmake-macro-equivalents)

---

## Prerequisites

Before starting, install the following on all developer machines and CI agents:

| Tool | Version | Notes |
|------|---------|-------|
| Bazel | 7.x (LTS) | Use Bazelisk for version pinning |
| Bazelisk | latest | Reads `.bazelversion`, downloads correct Bazel |
| MSVC | VS 2022 17.x | Windows toolchain; keep as primary for Windows |
| GCC 13+ / Clang 17+ | — | Linux toolchain |
| .NET SDK 9 | 9.0.x | Required for `atlas_clrscript` and C# tests |
| Python 3.9+ | — | Bazel dependency (bundled in most installs) |

Install Bazelisk (replaces `bazel` binary):

```bash
# Windows (via winget)
winget install Google.Bazelisk

# Linux
curl -Lo /usr/local/bin/bazel https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x /usr/local/bin/bazel
```

---

## Why Bazel

Key motivations for this codebase:

- **Accurate incremental builds.** Bazel hashes inputs, not timestamps. With 46 tests across 19 libraries and 10 server processes, unnecessary recompilation is frequent under CMake.
- **Shared remote cache.** All developers and CI share build/test artifacts. A clean CI checkout costs little once the cache is warm.
- **Precise test caching.** `bazel test //...` skips tests whose inputs have not changed. This is significant given the growing integration test suite.
- **Reproducible dependency closure.** FetchContent fetches at configure time and is not hermetic. Bazel locks all dependencies by content hash.
- **Dependency graph queries.** `bazel query 'deps(//src/server/baseapp)'` makes cross-server dependency analysis trivial.

---

## Migration Strategy

**Parallel operation throughout.** CMake is not removed until Bazel covers 100% of the build surface and all CI checks pass via Bazel. Both systems exist simultaneously during migration.

**Bottom-up library order.** Migrate libraries in dependency order (leaf libraries first) so each `BUILD` file can immediately reference already-migrated targets.

**Correctness gate.** After each phase, run both `ctest` and `bazel test //...` and diff the results. Do not proceed to the next phase if outputs diverge.

**No generated code changes.** The migration touches only build files. No source files are modified during migration.

---

## Phase 1 — Toolchain Bootstrap

**Goal:** A working Bazel invocation that can compile a trivial C++20 translation unit on both Windows and Linux.

**Estimated effort:** 3–5 days.

### 1.1 Pin the Bazel version

Create `.bazelversion` at the repository root:

```
7.4.1
```

Bazelisk reads this file and downloads the exact version. All developers automatically use the same Bazel binary.

### 1.2 Create MODULE.bazel

`MODULE.bazel` is the Bzlmod (modern) dependency declaration file. Place it at the repository root.

```python
module(
    name = "atlas",
    version = "0.1.0",
)

# C++ rules (built-in, but must declare)
bazel_dep(name = "rules_cc", version = "0.0.9")

# C# / .NET rules — for atlas_clrscript and C# tests
bazel_dep(name = "rules_dotnet", version = "0.17.0")

# Google Test
bazel_dep(name = "googletest", version = "1.14.0")

# Third-party C++ libraries (see Phase 2 for details)
bazel_dep(name = "pugixml", version = "1.14")
```

> **Note:** `rapidjson`, `zlib`, and `sqlite3` are not in the Bazel Central Registry at the required versions. They must be declared as `http_archive` in a `repos.bzl` extension (see Phase 2).

### 1.3 Create .bazelrc

`.bazelrc` applies flags globally. This replaces `AtlasCompilerOptions.cmake`.

```
# Common flags for all builds
build --cxxopt=-std=c++20
build --host_cxxopt=-std=c++20
build --enable_platform_specific_config

# Windows / MSVC
build:windows --cxxopt=/W4
build:windows --cxxopt=/permissive-
build:windows --cxxopt=/utf-8
build:windows --cxxopt=/Zc:__cplusplus
build:windows --cxxopt=/Zc:preprocessor
build:windows --cxxopt=/wd4100
build:windows --define=ATLAS_PLATFORM_WINDOWS=1

# Linux / GCC or Clang
build:linux --cxxopt=-Wall
build:linux --cxxopt=-Wextra
build:linux --cxxopt=-Wpedantic
build:linux --cxxopt=-Wno-unused-parameter
build:linux --cxxopt=-Wconversion
build:linux --cxxopt=-Wnon-virtual-dtor
build:linux --define=ATLAS_PLATFORM_LINUX=1

# Architecture defines (x86_64 assumed)
build --define=ATLAS_ARCH_X64=1

# Debug config
build:debug --compilation_mode=dbg
build:debug --define=ATLAS_DEBUG=1

# Release config
build:release --compilation_mode=opt
build:release --copt=-DNDEBUG

# RelWithDebInfo equivalent
build:hybrid --compilation_mode=opt
build:hybrid --copt=-g
build:hybrid --strip=never

# AddressSanitizer (Linux only)
build:asan --features=asan
build:asan --copt=-fsanitize=address
build:asan --linkopt=-fsanitize=address

# Remote cache (fill in after Phase 7)
# build:ci --remote_cache=grpcs://...

# Test output
test --test_output=errors
```

### 1.4 Verify with a smoke test

Create `src/lib/math/BUILD.bazel` (minimal — see Phase 3 for the full version):

```python
cc_library(
    name = "math",
    srcs = ["src/matrix4.cc", "src/quaternion.cc"],
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
```

Run:

```bash
bazel build //src/lib/math:math --config=debug
```

Fix any toolchain issues before proceeding. Common problems on Windows:
- Bazel cannot locate MSVC: set `BAZEL_VC` environment variable to the VS install path.
- `cl.exe` not on PATH: run from a VS Developer Command Prompt, or add to `.bazelrc`:
  ```
  build:windows --action_env=BAZEL_VC=C:/Program Files/Microsoft Visual Studio/2022/Community/VC
  ```

---

## Phase 2 — Third-Party Dependencies

**Goal:** All external dependencies are available as Bazel targets before migrating any Atlas library.

**Estimated effort:** 3–5 days.

The project uses five external C++ libraries. Their Bazel equivalents are:

| Library | CMake mechanism | Bazel approach |
|---------|----------------|----------------|
| googletest 1.15.2 | FetchContent (git) | `bazel_dep` in MODULE.bazel |
| pugixml 1.14 | FetchContent (git) | `bazel_dep` in MODULE.bazel |
| rapidjson (commit ab1842a) | FetchContent (git, header-only) | `http_archive` in `deps.bzl` |
| zlib 1.3.1 | FetchContent (git) | `http_archive` in `deps.bzl` |
| sqlite3 3.47.2 | FetchContent (amalgamation) | `http_archive` with custom `BUILD` |

### 2.1 Create deps.bzl for non-registry dependencies

```python
# deps.bzl — loaded by MODULE.bazel via use_repo_rule
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

def atlas_third_party_deps():
    # rapidjson — header-only, no build step needed
    http_archive(
        name = "rapidjson",
        url = "https://github.com/Tencent/rapidjson/archive/ab1842a2dae061284c0a62dca1cc6d5e7e37e346.tar.gz",
        sha256 = "<compute with shasum -a 256>",
        build_file = "//third_party:rapidjson.BUILD",
    )

    # zlib 1.3.1
    http_archive(
        name = "zlib",
        url = "https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz",
        sha256 = "17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c",
        build_file = "//third_party:zlib.BUILD",
    )

    # sqlite3 3.47.2 amalgamation
    http_archive(
        name = "sqlite3",
        url = "https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip",
        sha256 = "<compute>",
        build_file = "//third_party:sqlite3.BUILD",
    )
```

### 2.2 Write BUILD files for vendored deps

**`third_party/rapidjson.BUILD`** — header-only:

```python
cc_library(
    name = "rapidjson",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
```

**`third_party/zlib.BUILD`**:

```python
cc_library(
    name = "zlib",
    srcs = [
        "adler32.c", "compress.c", "crc32.c", "deflate.c",
        "gzclose.c", "gzlib.c", "gzread.c", "gzwrite.c",
        "infback.c", "inffast.c", "inflate.c", "inftrees.c",
        "trees.c", "uncompr.c", "zutil.c",
    ],
    hdrs = ["zlib.h", "zconf.h"],
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-Wno-implicit-function-declaration"],
    }),
    visibility = ["//visibility:public"],
)
```

**`third_party/sqlite3.BUILD`**:

```python
cc_library(
    name = "sqlite3",
    srcs = ["sqlite3.c"],
    hdrs = ["sqlite3.h", "sqlite3ext.h"],
    copts = ["-DSQLITE_THREADSAFE=1"],
    visibility = ["//visibility:public"],
)
```

### 2.3 MySQL (optional dependency)

MySQL is system-installed and not in any registry. Use a `configure_make` rule or a manual `cc_library` wrapping the system installation. Disable by default — mirror the CMake `ATLAS_DB_MYSQL=OFF` default via a build flag:

```python
# .bazelrc addition
build --define=ATLAS_DB_MYSQL=0

# BUILD.bazel usage
cc_library(
    name = "atlas_db_mysql",
    srcs = select({
        "//:db_mysql_enabled": ["src/mysql_database.cc", ...],
        "//conditions:default": [],
    }),
    ...
)
```

Define the config_setting in the root `BUILD.bazel`:

```python
config_setting(
    name = "db_mysql_enabled",
    define_values = {"ATLAS_DB_MYSQL": "1"},
)
```

### 2.4 .NET / hostfxr

The CLR host is a system dependency located by `FindDotNet.cmake`. In Bazel, create a repository rule that locates the .NET SDK installation:

```python
# tools/dotnet/dotnet_repository.bzl
def _dotnet_sdk_impl(ctx):
    # Probe DOTNET_ROOT or standard install paths
    ...

dotnet_sdk = repository_rule(
    implementation = _dotnet_sdk_impl,
    environ = ["DOTNET_ROOT", "PATH"],
)
```

This is the most complex dependency. It should be prototyped early and can be unblocked by hardcoding the path during development:

```python
# Temporary during development — replace with dotnet_sdk rule
cc_library(
    name = "dotnet_nethost",
    hdrs = ["C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/9.0.0/runtimes/win-x64/native/nethost.h"],
    srcs = [],
    linkopts = ["C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/9.0.0/runtimes/win-x64/native/libnethost.lib"],
    visibility = ["//visibility:public"],
)
```

---

## Phase 3 — Core Libraries (Bottom-Up)

**Goal:** All libraries in `src/lib/` have `BUILD.bazel` files and build cleanly.

**Estimated effort:** 5–7 days.

Migrate in strict dependency order. Each row depends only on libraries above it.

```
Tier 0: atlas_platform_config
Tier 1: atlas_math, atlas_foundation
Tier 2: atlas_platform, atlas_serialization, atlas_script
Tier 3: atlas_network, atlas_entitydef
Tier 4: atlas_coro, atlas_db_xml, atlas_db_sqlite
Tier 5: atlas_db, atlas_clrscript
Tier 6: atlas_server, atlas_engine (shared)
```

### Pattern: standard cc_library

All Atlas libraries use the same pattern:

```python
cc_library(
    name = "atlas_foundation",
    srcs = [
        "src/error.cc",
        "src/string_utils.cc",
        "src/log.cc",
        "src/log_sinks.cc",
        "src/runtime.cc",
        "src/clock.cc",
        "src/timer_queue.cc",
        "src/pool_allocator.cc",
        "src/memory_tracker.cc",
    ],
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    deps = [
        "//src/lib/platform_config:atlas_platform_config",
        "@platforms//os:windows",  # not a dep — see select() below
    ],
    copts = select({
        "@platforms//os:windows": ["/DATLAS_PLATFORM_WINDOWS"],
        "//conditions:default": ["-DATLAS_PLATFORM_LINUX"],
    }),
    visibility = ["//visibility:public"],
)
```

### Platform-specific source files using select()

`atlas_platform` is the most complex library due to six platform-specific files:

```python
cc_library(
    name = "atlas_platform",
    srcs = [
        "src/filesystem.cc",
        "src/io_poller.cc",
        "src/io_poller_select.cc",
        "src/threading.cc",
        "src/signal_handler.cc",
    ] + select({
        "@platforms//os:windows": [
            "src/io_poller_wsapoll.cc",
            "src/threading_windows.cc",
            "src/dynamic_library_windows.cc",
        ],
        "@platforms//os:linux": [
            "src/io_poller_epoll.cc",
            "src/threading_linux.cc",
            "src/dynamic_library_linux.cc",
        ],
    }),
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    linkopts = select({
        "@platforms//os:windows": ["-lws2_32"],
        "@platforms//os:linux": ["-ldl"],
        "//conditions:default": [],
    }),
    deps = ["//src/lib/foundation:atlas_foundation"],
    visibility = ["//visibility:public"],
)
```

For optional `io_uring` on Linux:

```python
config_setting(
    name = "linux_with_uring",
    constraint_values = ["@platforms//os:linux"],
    define_values = {"ATLAS_USE_IOURING": "1"},
)

# In atlas_platform srcs select():
"//src/lib/platform:linux_with_uring": [
    "src/io_poller_io_uring.cc",
],
```

### atlas_clrscript platform files

```python
srcs = [
    "src/base_native_provider.cc",
    "src/clr_bootstrap.cc",
    "src/clr_error.cc",
    "src/clr_host.cc",
    "src/clr_hot_reload.cc",
    "src/clr_object.cc",
    "src/clr_object_registry.cc",
    "src/clr_script_engine.cc",
    "src/file_watcher.cc",
    "src/native_api_provider.cc",
    "src/clr_marshal.cc",
] + select({
    "@platforms//os:windows": ["src/clr_host_windows.cc"],
    "@platforms//os:linux":   ["src/clr_host_linux.cc"],
}),
```

### atlas_engine shared library

This is the only `cc_shared_library` in the project. It exports C symbols for C# P/Invoke:

```python
cc_shared_library(
    name = "atlas_engine",
    roots = ["//src/lib/clrscript:atlas_clrscript"],
    exports_filter = ["//src/lib/clrscript/..."],
    user_link_flags = select({
        "@platforms//os:linux": ["-Wl,-fvisibility=hidden"],
        "//conditions:default": [],
    }),
    visibility = ["//visibility:public"],
)
```

### Cross-library include paths

Several server components include headers from `src/server/` (cross-server message definitions). Bazel enforces strict include hygiene. Expose these via a dedicated header-only target:

```python
# src/server/BUILD.bazel
cc_library(
    name = "server_hdrs",
    hdrs = glob(["**/*.h"]),
    includes = ["."],
    visibility = ["//visibility:public"],
)
```

Then add `"//src/server:server_hdrs"` to any target that needs cross-server includes.

---

## Phase 4 — Server Components

**Goal:** All server executables and their companion libraries build with Bazel.

**Estimated effort:** 3–4 days.

Each server component follows the same two-target pattern (library + executable). Example for `machined`:

```python
# src/server/machined/BUILD.bazel

cc_library(
    name = "atlas_machined_lib",
    srcs = [
        "process_registry.cc",
        "listener_manager.cc",
        "watcher_forwarder.cc",
        "machined_app.cc",
    ],
    hdrs = glob(["*.h"]),
    deps = ["//src/lib/server:atlas_server"],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "machined",
    srcs = ["main.cc"],
    deps = [":atlas_machined_lib"],
)
```

Server components by dependency tier:

```
machined:     deps [atlas_server]
baseappmgr:   deps [atlas_foundation, atlas_network, atlas_server]
dbapp:        deps [atlas_foundation, atlas_network, atlas_db, atlas_db_xml, atlas_entitydef, atlas_server]
loginapp:     deps [atlas_foundation, atlas_network, atlas_server, atlas_coro]
baseapp:      deps [atlas_foundation, atlas_network, atlas_db, atlas_entitydef, atlas_server, atlas_clrscript]
echoapp:      deps [atlas_server]
```

Stub components (`cellapp`, `cellappmgr`, `dbappmgr`, `reviver`) have no source files. Create empty `BUILD.bazel` files with a comment noting they are placeholders.

**Client:**

```python
# src/client/BUILD.bazel
cc_library(
    name = "atlas_client_lib",
    srcs = ["client_app.cc", "client_native_provider.cc"],
    hdrs = glob(["*.h"]),
    deps = [
        "//src/lib/foundation:atlas_foundation",
        "//src/lib/network:atlas_network",
        "//src/lib/serialization:atlas_serialization",
        "//src/lib/entitydef:atlas_entitydef",
        "//src/lib/clrscript:atlas_clrscript",
        "//src/lib/platform:atlas_platform",
        "//src/lib/server:atlas_server",
        "//src/lib/db:atlas_db",
        "//src/server:server_hdrs",
    ],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "atlas_client",
    srcs = ["main.cc"],
    deps = [":atlas_client_lib"],
)
```

---

## Phase 5 — Tests

**Goal:** All 46 tests (39 unit + 7 integration) run under `bazel test //...`.

**Estimated effort:** 4–5 days.

### 5.1 Unit test pattern

Each test maps to one `cc_test` rule:

```python
# tests/unit/BUILD.bazel (excerpt)

cc_test(
    name = "test_math",
    srcs = ["test_math.cpp"],
    deps = [
        "//src/lib/math:atlas_math",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "test_event_dispatcher",
    srcs = ["test_event_dispatcher.cpp"],
    deps = [
        "//src/lib/network:atlas_network",
        "@googletest//:gtest_main",
    ],
)
```

Generate all 39 unit test targets in `tests/unit/BUILD.bazel`.

### 5.2 Integration test considerations

The 7 integration tests (`test_tcp_echo_roundtrip`, `test_machined_registration`, etc.) spawn child processes and open sockets. Bazel sandboxes tests by default. Required adjustments:

```python
cc_test(
    name = "test_machined_registration",
    srcs = ["test_machined_registration.cpp"],
    deps = [
        "//src/server/machined:atlas_machined_lib",
        "@googletest//:gtest_main",
    ],
    # Integration tests need network access and may spawn subprocesses
    tags = ["integration", "local"],
    # Disable sandboxing for process-spawning tests
    local = True,
    # Provide server binaries as runfiles
    data = [
        "//src/server/machined:machined",
    ],
)
```

The `data` attribute makes the binary available as a runfile. Update test code to locate binaries via `bazel_tools/cpp/runfiles`:

```cpp
#include "tools/cpp/runfiles/runfiles.h"
// Use runfiles->Rlocation("atlas/src/server/machined/machined")
// instead of hard-coded relative paths
```

### 5.3 CLR integration tests

Tests that exercise C# code (`test_clr_host`, `test_clr_script_engine`, etc.) require the `atlas_engine` shared library and the C# runtime assemblies in the runfiles tree. Use `rules_dotnet` to build the C# projects and include them as `data` dependencies.

This is the most complex part of the test migration. Placeholder approach during development:

```python
cc_test(
    name = "test_clr_host",
    srcs = ["test_clr_host.cpp"],
    deps = ["//src/lib/clrscript:atlas_clrscript", "@googletest//:gtest_main"],
    data = [
        "//src/lib/engine:atlas_engine",
        "//src/csharp/Atlas.Runtime:atlas_runtime_dll",  # Phase 6
    ],
    env = {"DOTNET_ROOT": "/path/to/dotnet"},  # replace with repo rule
    tags = ["integration", "local"],
)
```

### 5.4 Test suite aliases

Add convenience aliases for running test groups:

```python
# tests/BUILD.bazel
test_suite(
    name = "unit_tests",
    tags = ["unit"],
)

test_suite(
    name = "integration_tests",
    tags = ["integration"],
)
```

Run with:
```bash
bazel test //tests:unit_tests
bazel test //tests:integration_tests
```

---

## Phase 6 — C# and CLR Integration

**Goal:** C# projects build via `rules_dotnet` and are available as Bazel targets.

**Estimated effort:** 5–7 days (highest risk phase).

### 6.1 Configure rules_dotnet

Add to `MODULE.bazel`:

```python
bazel_dep(name = "rules_dotnet", version = "0.17.0")

dotnet = use_extension("@rules_dotnet//dotnet:extensions.bzl", "dotnet")
dotnet.toolchain(dotnet_version = "9.0.100")
use_repo(dotnet, "dotnet_toolchains")

register_toolchains("@dotnet_toolchains//:all")
```

### 6.2 C# library targets

For each C# project (`Atlas.Runtime`, `Atlas.RuntimeTest`, `Atlas.SmokeTest`):

```python
# src/csharp/Atlas.Runtime/BUILD.bazel
load("@rules_dotnet//dotnet:defs.bzl", "csharp_library")

csharp_library(
    name = "atlas_runtime_dll",
    srcs = glob(["**/*.cs"]),
    target_framework = "net9.0",
    visibility = ["//visibility:public"],
)
```

### 6.3 Replacing atlas_build_csharp_project macro

The CMake macro `atlas_build_csharp_project()` calls `dotnet build` as a custom command. In Bazel this becomes a proper `csharp_library` or `csharp_binary` target with declared inputs and outputs — no shell-out required.

### 6.4 hostfxr repository rule

Replace the temporary hardcoded path with a proper repository rule:

```python
# tools/dotnet/dotnet_repository.bzl
def _find_dotnet_sdk(ctx):
    dotnet_root = ctx.os.environ.get("DOTNET_ROOT", "")
    if not dotnet_root:
        # Probe standard locations
        if ctx.os.name == "windows":
            dotnet_root = "C:/Program Files/dotnet"
        else:
            dotnet_root = "/usr/share/dotnet"

    sdk_version = "9.0.0"
    arch = "win-x64" if ctx.os.name == "windows" else "linux-x64"

    pack_path = "{}/packs/Microsoft.NETCore.App.Host.{}/{}/runtimes/{}/native".format(
        dotnet_root, arch, sdk_version, arch
    )

    ctx.file("BUILD.bazel", """
cc_library(
    name = "dotnet_nethost",
    hdrs = ["{pack_path}/nethost.h"],
    linkopts = ["{pack_path}/libnethost.lib"],
    visibility = ["//visibility:public"],
)
""".format(pack_path = pack_path))

dotnet_sdk = repository_rule(
    implementation = _find_dotnet_sdk,
    environ = ["DOTNET_ROOT", "DOTNET_SDK_VERSION"],
)
```

Register in `MODULE.bazel`:

```python
dotnet_sdk = use_repo_rule("//tools/dotnet:dotnet_repository.bzl", "dotnet_sdk")
dotnet_sdk(name = "dotnet_host")
```

---

## Phase 7 — CI and Remote Cache

**Goal:** CI uses Bazel exclusively; remote cache is configured to share artifacts across builds.

**Estimated effort:** 2–3 days.

### 7.1 CI workflow (GitHub Actions example)

```yaml
# .github/workflows/build.yml
- name: Install Bazelisk
  run: |
    curl -Lo bazel https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
    chmod +x bazel && sudo mv bazel /usr/local/bin/bazel

- name: Build
  run: bazel build //... --config=release

- name: Test
  run: bazel test //... --config=release --test_tag_filters=-integration
```

### 7.2 Remote cache setup

Options in order of setup cost:

| Option | Cost | Setup |
|--------|------|-------|
| GitHub Actions Cache (via `bazel-remote`) | Free | ~1 hour |
| BuildBuddy Cloud (free tier) | Free / paid | ~30 min |
| Self-hosted `bazel-remote` | Infrastructure cost | ~2 hours |

Add to `.bazelrc`:

```
# CI-only remote cache (set BAZEL_REMOTE_CACHE in CI environment)
build:ci --remote_cache=${BAZEL_REMOTE_CACHE}
build:ci --remote_upload_local_results=true
```

### 7.3 Parallel build tuning

```
# .bazelrc additions
build --jobs=auto          # use all available cores
build --local_ram_resources=HOST_RAM*.75
build --local_cpu_resources=HOST_CPUS*.75
```

---

## Phase 8 — CMake Removal

**Goal:** Remove all CMake files once Bazel is the sole build system.

**Prerequisites before removal:**
- [ ] `bazel build //...` completes without errors on Windows and Linux
- [ ] `bazel test //...` passes all tests (or matches the set that passes under CMake)
- [ ] CI uses only Bazel for at least two weeks with no regressions
- [ ] All developers have migrated their local workflows
- [ ] `CLAUDE.md`, `README.md`, and all docs updated to reference Bazel commands

**Files to remove:**

```
CMakeLists.txt                 (root)
CMakePresets.json
cmake/
  AtlasCompilerOptions.cmake
  AtlasMacros.cmake
  AtlasFindPackages.cmake
  FindDotNet.cmake
src/lib/*/CMakeLists.txt       (19 files)
src/server/*/CMakeLists.txt    (10 files)
src/client/CMakeLists.txt
src/client_sdk/CMakeLists.txt
src/tools/*/CMakeLists.txt
tests/CMakeLists.txt
tests/unit/CMakeLists.txt
tests/integration/CMakeLists.txt
```

**Update CLAUDE.md** build commands section:

```bash
# Build
bazel build //... --config=debug

# Test
bazel test //... --test_tag_filters=-integration

# Run specific server
bazel run //src/server/machined:machined
```

---

## Reference: Dependency Graph

Full library dependency order for migration:

```
atlas_platform_config   (no deps)
  └─ atlas_math         (platform_config)
  └─ atlas_foundation   (platform_config)
       └─ atlas_platform         (foundation)
       └─ atlas_serialization    (foundation, platform, pugixml, rapidjson)
       └─ atlas_script           (foundation)
            └─ atlas_network     (foundation, platform, serialization, zlib)
            └─ atlas_entitydef   (foundation, serialization)
                 └─ atlas_coro         (foundation, network)
                 └─ atlas_db_xml       (foundation, serialization, entitydef, network)
                 └─ atlas_db_sqlite    (foundation, entitydef, network, sqlite3)
                      └─ atlas_db      (foundation, network, entitydef, db_xml, db_sqlite, platform)
                      └─ atlas_clrscript (foundation, platform, math, script, entitydef, dotnet_host)
                           └─ atlas_server (foundation, network, platform, serialization, script,
                                            clrscript, entitydef)
                                └─ atlas_engine [SHARED] (clrscript, foundation, platform)
```

---

## Reference: Platform-Specific Files

All files requiring `select()` in Bazel:

| Library | Windows | Linux |
|---------|---------|-------|
| `atlas_platform` | `io_poller_wsapoll.cc`, `threading_windows.cc`, `dynamic_library_windows.cc` | `io_poller_epoll.cc`, `threading_linux.cc`, `dynamic_library_linux.cc`, *(optional)* `io_poller_io_uring.cc` |
| `atlas_clrscript` | `clr_host_windows.cc` | `clr_host_linux.cc` |
| `atlas_platform` linkopts | `-lws2_32` | `-ldl` |
| `atlas_engine` visibility | *(default)* | `-fvisibility=hidden` |

---

## Reference: CMake Macro Equivalents

| CMake macro | Bazel equivalent |
|-------------|-----------------|
| `atlas_add_library(T SOURCES ... DEPS ...)` | `cc_library(name=T, srcs=[...], deps=[...])` |
| `atlas_add_executable(T SOURCES ... DEPS ...)` | `cc_binary(name=T, srcs=[...], deps=[...])` |
| `atlas_add_test(T SOURCES ... DEPS ...)` | `cc_test(name=T, srcs=[...], deps=[..., "@googletest//:gtest_main"])` |
| `atlas_build_csharp_project(dir)` | `csharp_library(...)` or `csharp_binary(...)` via rules_dotnet |
| `FetchContent_Declare` + `FetchContent_MakeAvailable` | `http_archive` in `deps.bzl` or `bazel_dep` in `MODULE.bazel` |
| `find_package(Threads)` | `@platforms//os:linux` + `-lpthread` in linkopts |
| `CMAKE_BUILD_TYPE=Debug` | `--config=debug` |
| `CMAKE_BUILD_TYPE=Release` | `--config=release` |
| `ATLAS_ENABLE_ASAN=ON` | `--config=asan` |
| `ctest --test-dir build` | `bazel test //...` |
| `cmake --preset debug-windows` | `bazel build //... --config=debug` (Windows auto-detected) |

---

## Effort Summary

| Phase | Description | Days |
|-------|-------------|------|
| 1 | Toolchain bootstrap | 3–5 |
| 2 | Third-party dependencies | 3–5 |
| 3 | Core libraries | 5–7 |
| 4 | Server components | 3–4 |
| 5 | Tests | 4–5 |
| 6 | C# / CLR integration | 5–7 |
| 7 | CI and remote cache | 2–3 |
| 8 | CMake removal | 1 |
| **Total** | | **26–37 days** |

The highest-risk phases are Phase 2 (hostfxr repository rule) and Phase 6 (rules_dotnet integration). Both involve underdocumented Bazel ecosystems. Budget extra time for each and prototype them in parallel with Phase 3.
