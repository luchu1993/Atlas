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
FetchContent_Declare(
  rapidjson
  URL https://github.com/Tencent/rapidjson/archive/ab1842a2dae061284c0a62dca1cc6d5e7e37e346.tar.gz
)

# ── zlib 1.3.1 ───────────────────────────────────────────────────────────────
FetchContent_Declare(
  zlib
  URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
  URL_HASH SHA256=17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c
)

# ── sqlite3 3.47.2 (amalgamation, no CMakeLists.txt) ────────────────────────
FetchContent_Declare(
  sqlite3
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip
)

# ── Tracy 0.11.1 ─────────────────────────────────────────────────────────────
# Held back from upstream's current release (0.13.x at the time of
# writing) because Tracy-CSharp — the managed-side binding the server
# uses to surface C# zones in the same Tracy timeline — has only been
# updated through Tracy 0.11.1. Tracy's wire protocol changes between
# minor versions; mismatched native and managed clients connect but
# silently lose zones. Until Tracy-CSharp catches up, this version is
# the ceiling. Bump only in lockstep with the Tracy.CSharp NuGet
# package referenced from Atlas.Runtime.csproj.
FetchContent_Declare(
  tracy
  URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.11.1.tar.gz
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
FetchContent_GetProperties(rapidjson)
if(NOT rapidjson_POPULATED)
  FetchContent_Populate(rapidjson)
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

# Tracy — only populated when the profiler is enabled. Tracy ships its
# own no-op headers when TRACY_ENABLE is undefined, so even with the
# library linked, ATLAS_PROFILE_ENABLED=0 keeps everything inert. We
# still skip the fetch entirely in that mode to spare CI download time.
if(ATLAS_ENABLE_PROFILER)
  set(TRACY_ENABLE      ON  CACHE BOOL "" FORCE)
  if(ATLAS_PROFILER_ON_DEMAND)
    set(TRACY_ON_DEMAND ON  CACHE BOOL "" FORCE)
  else()
    set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
  endif()
  # Keep Tracy's own ancillary tools out of the build — we only need
  # the in-process client library. The viewer is a separate desktop
  # download.
  set(TRACY_NO_BROADCAST     OFF CACHE BOOL "" FORCE)
  set(TRACY_NO_CONTEXT_SWITCH OFF CACHE BOOL "" FORCE)
  set(TRACY_STATIC           ON  CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(tracy)
  if(TARGET TracyClient AND NOT TARGET Tracy::TracyClient)
    add_library(Tracy::TracyClient ALIAS TracyClient)
  endif()
endif()

# sqlite3 — build manually from amalgamation
FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
  FetchContent_Populate(sqlite3)
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
