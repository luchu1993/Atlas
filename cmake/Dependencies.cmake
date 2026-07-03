# Dependencies.cmake
#
# Third-party dependencies via FetchContent.

include(FetchContent)

# ── Google Test 1.15.2 ───────────────────────────────────────────────────────
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  FIND_PACKAGE_ARGS NAMES GTest
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# ── pugixml 1.14 ─────────────────────────────────────────────────────────────
FetchContent_Declare(
  pugixml
  URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz
  URL_HASH SHA256=2f10e276870c64b1db6809050a75e11a897a8d7456c4be5c6b2e35a11168a015
)

# ── rapidjson (header-only, pinned commit) ───────────────────────────────────
# Header-only: skip rapidjson's own CMakeLists.txt (which builds tests /
# examples) by pointing SOURCE_SUBDIR at a non-existent path.
FetchContent_Declare(
  rapidjson
  URL https://github.com/Tencent/rapidjson/archive/ab1842a2dae061284c0a62dca1cc6d5e7e37e346.tar.gz
  SOURCE_SUBDIR cmake-noop
)

# ── zlib 1.3.1 ───────────────────────────────────────────────────────────────
FetchContent_Declare(
  zlib
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
  URL_HASH SHA256=17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c
)

# ── sqlite3 3.47.2 (amalgamation, no CMakeLists.txt) ────────────────────────
# SOURCE_SUBDIR points to a non-existent path so FetchContent_MakeAvailable
# skips add_subdirectory — we build the static lib manually below.
FetchContent_Declare(
  sqlite3
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip
  SOURCE_SUBDIR cmake-noop
)

# ── mimalloc 2.3.1 (optional heap backend) ───────────────────────────────────
# Fetched only when ATLAS_HEAP_ALLOCATOR=mimalloc. Built as a shared
# library so a single mi_heap instance backs every Atlas binary —
# under OBJECT-library propagation each DLL has its own copy of
# atlas::HeapAlloc, but all of them must call into one mimalloc state
# for cross-DLL pointer freeing to remain safe (see heap.h's
# cross-DLL invariant note).
#
# MI_OVERRIDE=OFF: Atlas owns the operator new / delete override
# itself (heap.cc), so mimalloc must NOT install its own — a
# duplicate override would either lose to ours at link time (best
# case) or, on platforms that resolve weak symbols differently,
# fragment the heap policy (worst case).
if(ATLAS_HEAP_ALLOCATOR STREQUAL "mimalloc")
  FetchContent_Declare(
    mimalloc
    URL https://github.com/microsoft/mimalloc/archive/refs/tags/v2.3.1.tar.gz
  )
endif()

# ── Jolt 5.2.0 ───────────────────────────────────────────────────────────────
if(ATLAS_ENABLE_JOLT)
  FetchContent_Declare(
    Jolt
    URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.2.0.tar.gz
    URL_HASH SHA256=f478afe3050c885e21403748e10ab18e3e8df8b0982c540e75f1e078ef8b2c88
    SOURCE_SUBDIR Build
  )
endif()

# ── recastnavigation 1.6.0 ───────────────────────────────────────────────────
if(ATLAS_ENABLE_RECAST)
  FetchContent_Declare(
    recastnavigation
    URL https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz
  )
endif()

# ── Tracy 0.13.1 ─────────────────────────────────────────────────────────────
# Pinned in lockstep with the Tracy-NET 0.13.2 NuGet package referenced
# from Atlas.Runtime.csproj — Tracy's wire protocol changes between
# minor versions, and a mismatched native↔managed pair connects but
# silently drops zones. Tracy-NET (xLuxy fork of the original
# clibequilibrium/Tracy-CSharp) tracks upstream Tracy actively; if it
# stalls, prefer freezing here over splitting native and managed
# protocol versions.
FetchContent_Declare(
  tracy
  URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.13.1.tar.gz
)

# ── Make available ───────────────────────────────────────────────────────────

# googletest — ships CMakeLists.txt
FetchContent_MakeAvailable(googletest)

# pugixml — ships CMakeLists.txt
# pugixml unconditionally calls include(CTest), which pollutes the IDE with
# a CTestDashboardTargets folder (Continuous/Experimental/Nightly…). We
# never submit to CDash, so shadow the stock CTest module with an empty
# stub on CMAKE_MODULE_PATH for the duration of the pugixml fetch.
set(_atlas_ctest_stub_dir "${CMAKE_BINARY_DIR}/_atlas_cmake_stubs")
file(WRITE "${_atlas_ctest_stub_dir}/CTest.cmake"
  "# Stub: suppresses CTest dashboard targets inside pugixml.\n")
list(PREPEND CMAKE_MODULE_PATH "${_atlas_ctest_stub_dir}")
FetchContent_MakeAvailable(pugixml)
list(REMOVE_ITEM CMAKE_MODULE_PATH "${_atlas_ctest_stub_dir}")
unset(_atlas_ctest_stub_dir)

# rapidjson — header-only, avoid its complex CMakeLists.txt
FetchContent_MakeAvailable(rapidjson)
if(NOT TARGET rapidjson)
  add_library(rapidjson INTERFACE)
  target_include_directories(rapidjson INTERFACE "${rapidjson_SOURCE_DIR}/include")
endif()

# zlib — ships CMakeLists.txt
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)
# zlib's CMakeLists.txt creates 'zlibstatic'; alias for consistency
if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# mimalloc — only populated when selected. The shared-only build keeps
# one allocator instance per process (see Declare comment above).
if(ATLAS_HEAP_ALLOCATOR STREQUAL "mimalloc")
  set(MI_OVERRIDE      OFF CACHE BOOL "" FORCE)
  # MI_WIN_REDIRECT pulls in mimalloc-redirect.dll, an extra
  # process-injection helper that mimalloc loads at runtime to hook
  # the system malloc surface. We don't want that — atlas::HeapAlloc
  # is the explicit entry point here, and shipping redirect.dll
  # alongside every Atlas binary just to never fire it is dead weight.
  set(MI_WIN_REDIRECT  OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_SHARED  ON  CACHE BOOL "" FORCE)
  set(MI_BUILD_STATIC  OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_OBJECT  OFF CACHE BOOL "" FORCE)
  set(MI_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(mimalloc)
  if(TARGET mimalloc AND NOT TARGET Mimalloc::Mimalloc)
    add_library(Mimalloc::Mimalloc ALIAS mimalloc)
  endif()
endif()

# Tracy — only populated when the profiler is enabled. Tracy ships its
# own no-op headers when TRACY_ENABLE is undefined, so even with the
# library linked, ATLAS_PROFILE_ENABLED=0 keeps everything inert. We
# still skip the fetch entirely in that mode to spare CI download time.
#
# Built as SHARED so the same TracyClient.dll/so backs both the C++
# call sites (linked via Tracy::TracyClient) and the managed P/Invoke
# surface in Tracy-NET ([DllImport("TracyClient")]). A single Tracy
# instance per process is what makes the unified C++/C# timeline work —
# two clients would publish to two different listener ports and split
# the trace.
if(ATLAS_ENABLE_PROFILER)
  set(TRACY_ENABLE      ON  CACHE BOOL "" FORCE)
  if(ATLAS_PROFILER_ON_DEMAND)
    set(TRACY_ON_DEMAND ON  CACHE BOOL "" FORCE)
  else()
    set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
  endif()
  # Keep Tracy's ancillary tools out of this dependency target; the
  # top-level ATLAS_BUILD_TRACY_VIEWER option deploys matched CLI exes.
  set(TRACY_NO_BROADCAST     OFF CACHE BOOL "" FORCE)
  set(TRACY_NO_CONTEXT_SWITCH OFF CACHE BOOL "" FORCE)
  set(TRACY_STATIC           OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(tracy)
  if(TARGET TracyClient AND NOT TARGET Tracy::TracyClient)
    add_library(Tracy::TracyClient ALIAS TracyClient)
  endif()
endif()

# Jolt — disable Jolt's own sample / test / viewer targets and stop it from
# overriding our compile flags.
if(ATLAS_ENABLE_JOLT)
  set(TARGET_HELLO_WORLD              OFF CACHE BOOL "" FORCE)
  set(TARGET_PERFORMANCE_TEST         OFF CACHE BOOL "" FORCE)
  set(TARGET_SAMPLES                  OFF CACHE BOOL "" FORCE)
  set(TARGET_UNIT_TESTS               OFF CACHE BOOL "" FORCE)
  set(TARGET_VIEWER                   OFF CACHE BOOL "" FORCE)
  set(OVERRIDE_CXX_FLAGS              OFF CACHE BOOL "" FORCE)
  set(INTERPROCEDURAL_OPTIMIZATION    OFF CACHE BOOL "" FORCE)
  set(CROSS_PLATFORM_DETERMINISTIC    OFF CACHE BOOL "" FORCE)
  set(DOUBLE_PRECISION                OFF CACHE BOOL "" FORCE)
  set(ENABLE_ALL_WARNINGS             OFF CACHE BOOL "" FORCE)
  # Atlas links against the dynamic MSVC runtime (/MDd, /MD). Jolt defaults to
  # the static runtime, which would trigger LNK2038 RuntimeLibrary mismatch.
  set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(Jolt)
endif()

# recastnavigation — build only the Recast + Detour static libs; the demo needs
# SDL2 and the tests need Catch2, neither of which Atlas ships.
if(ATLAS_ENABLE_RECAST)
  set(RECASTNAVIGATION_DEMO     OFF CACHE BOOL "" FORCE)
  set(RECASTNAVIGATION_TESTS    OFF CACHE BOOL "" FORCE)
  set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
  # 1.6.0 still declares cmake_minimum_required(VERSION 3.1); newer CMake removed
  # that compat, so opt this subtree into 3.5 policies.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  FetchContent_MakeAvailable(recastnavigation)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

# sqlite3 — build manually from amalgamation
FetchContent_MakeAvailable(sqlite3)
if(NOT TARGET sqlite3)
  add_library(sqlite3 STATIC
    "${sqlite3_SOURCE_DIR}/sqlite3.c"
  )
  target_include_directories(sqlite3 PUBLIC "${sqlite3_SOURCE_DIR}")
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
  )
  # Suppress all warnings for third-party code
  if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
  else()
    target_compile_options(sqlite3 PRIVATE -w)
  endif()
endif()

# ── MariaDB Connector/C 3.4.9 (MySQL backend, gated by ATLAS_DB_MYSQL) ───────
# LGPL client lib, linked only into the atlas_db_mysql plugin (a shared lib, so
# the LGPL boundary stays dynamic). Cloned via git for its bundled zlib
# submodule (GitHub tarballs omit submodules). SSL/curl off for the first cut;
# TLS + caching_sha2 auth are a later hardening step.
if(ATLAS_DB_MYSQL)
  set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(CONC_WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(WITH_CURL OFF CACHE BOOL "" FORCE)
  set(CONC_WITH_MSI OFF CACHE BOOL "" FORCE)
  # A non-empty INSTALL_PLUGINDIR keeps the connector's INSTALL(TARGETS) rules
  # valid for any plugin left dynamic (e.g. zstd on Linux). We never run install
  # and only build the static mariadbclient, so those .so plugins are never made.
  set(INSTALL_PLUGINDIR "lib/mariadb/plugin" CACHE STRING "" FORCE)
  # Compile the MySQL auth plugins we need statically into libmariadb (self-
  # contained client) and drop the MariaDB-specific ones (parsec/ed25519/gssapi)
  # — parsec also mis-orders the WinSock headers under MSVC.
  set(CLIENT_PLUGIN_DIALOG STATIC CACHE STRING "" FORCE)
  set(CLIENT_PLUGIN_CACHING_SHA2_PASSWORD STATIC CACHE STRING "" FORCE)
  set(CLIENT_PLUGIN_SHA256_PASSWORD STATIC CACHE STRING "" FORCE)
  set(CLIENT_PLUGIN_MYSQL_CLEAR_PASSWORD STATIC CACHE STRING "" FORCE)
  set(CLIENT_PLUGIN_PVIO_SHMEM STATIC CACHE STRING "" FORCE)
  set(CLIENT_PLUGIN_CLIENT_ED25519 OFF CACHE STRING "" FORCE)
  set(CLIENT_PLUGIN_PARSEC OFF CACHE STRING "" FORCE)
  set(CLIENT_PLUGIN_AUTH_GSSAPI_CLIENT OFF CACHE STRING "" FORCE)
  # TLS is mandatory in the connector: Schannel is built into Windows, OpenSSL
  # on Unix (libssl-dev on CI). caching_sha2 auth needs it in production anyway.
  if(WIN32)
    set(WITH_SSL SCHANNEL CACHE STRING "" FORCE)
  else()
    set(WITH_SSL OPENSSL CACHE STRING "" FORCE)
  endif()
  # Reuse Atlas's already-built zlib so the connector's bundled copy doesn't
  # redefine the zlibstatic target.
  set(WITH_EXTERNAL_ZLIB ON CACHE BOOL "" FORCE)
  set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
  set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)
  set(ZLIB_LIBRARIES zlibstatic CACHE STRING "" FORCE)
  set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "" FORCE)
  FetchContent_Declare(
    mariadb_connector_c
    GIT_REPOSITORY https://github.com/mariadb-corporation/mariadb-connector-c.git
    GIT_TAG v3.4.9
    EXCLUDE_FROM_ALL
    SYSTEM
  )
  FetchContent_MakeAvailable(mariadb_connector_c)
  # mariadbclient doesn't export its public headers as INTERFACE includes, so
  # surface them for the atlas_db_mysql backend. mariadb_version.h/ma_config.h
  # are generated into the build tree, so both dirs are needed.
  set(ATLAS_MARIADB_INCLUDE_DIR
      "${mariadb_connector_c_SOURCE_DIR}/include"
      "${mariadb_connector_c_BINARY_DIR}/include"
      CACHE INTERNAL "MariaDB Connector/C public headers")
  # The connector defines these only at top level (guarded by NOT IS_SUBPROJECT),
  # so as a FetchContent subproject <windows.h> pulls the legacy <winsock.h> and
  # clashes with winsock2. Re-apply them to the object lib that holds its sources.
  if(WIN32 AND TARGET mariadb_obj)
    target_compile_definitions(mariadb_obj PRIVATE WIN32_LEAN_AND_MEAN NOGDI)
  endif()
endif()
