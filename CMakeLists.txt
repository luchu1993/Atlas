cmake_minimum_required(VERSION 3.28)
project(Atlas VERSION 0.1.0 LANGUAGES C CXX)

# ── C++ standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build every static library position-independent so they can be linked
# into the SHARED plugin libraries (atlas_db_sqlite_plugin,
# atlas_db_xml_plugin, atlas_engine). Without this GCC's default non-PIC
# archives (sqlite3, pugixml, gtest, …) fail the ld link with
# "relocation R_X86_64_PC32 ... can not be used when making a shared
# object; recompile with -fPIC". A no-op on MSVC / Windows.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export compile_commands.json for clangd / IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────────────────────────
option(ATLAS_BUILD_TESTS "Build unit and integration tests" ON)
option(ATLAS_BUILD_CSHARP "Build C# projects via dotnet" ON)
option(ATLAS_DB_MYSQL "Enable MySQL database backend" OFF)
option(ATLAS_USE_IOURING "Enable io_uring I/O poller (Linux only)" OFF)
option(ATLAS_ENABLE_PROFILER "Enable profiler instrumentation (Tracy backend)" ON)
option(ATLAS_PROFILER_ON_DEMAND
       "Tracy ON_DEMAND mode — instrumentation goes inert until a client connects"
       ON)
option(ATLAS_BUILD_TRACY_VIEWER
       "Also build the Tracy viewer GUI from source into bin/<build_dir>/tools/. \
Off by default because the viewer pulls in many vendored dependencies \
(ImGui, GLFW, libcurl, FreeType, Capstone, …) and adds 30–60 s to a clean \
configure+build."
       OFF)
if(ATLAS_BUILD_TRACY_VIEWER AND NOT ATLAS_ENABLE_PROFILER)
  message(FATAL_ERROR
    "ATLAS_BUILD_TRACY_VIEWER=ON requires ATLAS_ENABLE_PROFILER=ON — the "
    "viewer is built from the same Tracy source FetchContent that the "
    "profiler client pulls in.")
endif()

# Heap allocator backend used by atlas::Heap (foundation/heap.h). Drives
# what RawAlloc/RawFree resolve to in heap.cc; user code stays
# allocator-agnostic. Adding a new backend is a three-step change:
# extend the STRINGS list, plumb a new ATLAS_HEAP_<NAME> compile
# definition in cmake/AtlasCompilerOptions.cmake, and add the
# matching #elif branch in heap.cc.
set(ATLAS_HEAP_ALLOCATOR "std" CACHE STRING
    "Heap allocator backend for atlas::Heap (std | mimalloc)")
set_property(CACHE ATLAS_HEAP_ALLOCATOR PROPERTY STRINGS std mimalloc)
set(_atlas_valid_heap_allocators std mimalloc)
if(NOT ATLAS_HEAP_ALLOCATOR IN_LIST _atlas_valid_heap_allocators)
  message(FATAL_ERROR
    "ATLAS_HEAP_ALLOCATOR='${ATLAS_HEAP_ALLOCATOR}' is not supported. "
    "Valid values: ${_atlas_valid_heap_allocators}")
endif()
unset(_atlas_valid_heap_allocators)

# ── CMake modules ────────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(AtlasCompilerOptions)
include(Dependencies)
include(FindDotNet)
include(AtlasRuntimeDeploy)
include(AtlasOutputDirectory)

if(ATLAS_BUILD_CSHARP)
  include(AtlasDotNetBuild)
endif()

# ── Tracy viewer GUI (optional) ──────────────────────────────────────────────
# We ship the viewer alongside Atlas binaries by downloading the
# upstream prebuilt archive matched to our pinned Tracy version, then
# extracting Tracy.exe into bin/<build_dir>/tools/. Building from
# source via add_subdirectory(${tracy_SOURCE_DIR}/profiler) sounds
# cleaner but Tracy's profiler CMakeLists invokes an embed helper as
# `${CMAKE_CURRENT_BINARY_DIR}/embed`, which CMake strips to a bare
# `embed` once it equals the rule's working directory — Windows CMD
# then fails to find it because "." isn't in PATH. The prebuilt path
# sidesteps that without patching Tracy upstream.
#
# Linux / macOS prebuilts aren't published officially by Tracy; on
# those platforms a manual `cmake -S _deps/tracy-src/profiler` is the
# documented path. That's flagged in docs/operations/tracy_usage.md.
if(ATLAS_BUILD_TRACY_VIEWER)
  if(WIN32)
    set(_atlas_tracy_viewer_url
        "https://github.com/wolfpld/tracy/releases/download/v0.13.1/windows-0.13.1.zip")
    set(_atlas_tracy_viewer_archive "${CMAKE_BINARY_DIR}/_atlas_tracy_viewer.zip")
    set(_atlas_tracy_viewer_dir "${CMAKE_BINARY_DIR}/_atlas_tracy_viewer")
    set(_atlas_tracy_viewer_marker "${_atlas_tracy_viewer_dir}/tracy-profiler.exe")
    if(NOT EXISTS "${_atlas_tracy_viewer_marker}")
      message(STATUS "Atlas: downloading Tracy viewer ${_atlas_tracy_viewer_url}")
      file(DOWNLOAD "${_atlas_tracy_viewer_url}" "${_atlas_tracy_viewer_archive}"
           SHOW_PROGRESS STATUS _atlas_dl_status)
      list(GET _atlas_dl_status 0 _atlas_dl_code)
      if(NOT _atlas_dl_code EQUAL 0)
        message(FATAL_ERROR "Tracy viewer download failed: ${_atlas_dl_status}")
      endif()
      file(MAKE_DIRECTORY "${_atlas_tracy_viewer_dir}")
      file(ARCHIVE_EXTRACT INPUT "${_atlas_tracy_viewer_archive}"
           DESTINATION "${_atlas_tracy_viewer_dir}")
    endif()
    if(NOT EXISTS "${_atlas_tracy_viewer_marker}")
      message(FATAL_ERROR
        "Tracy viewer archive extracted but tracy-profiler.exe not found "
        "at ${_atlas_tracy_viewer_marker} — upstream archive layout may "
        "have changed. Check ${_atlas_tracy_viewer_dir}.")
    endif()
    # The 0.13.1 release ships the GUI plus a few CLI helpers
    # (capture, csvexport, import-chrome, import-fuchsia, update).
    # Deploy the lot — they're tiny, all share the same provenance,
    # and operators routinely use capture / csvexport alongside the
    # GUI for non-interactive collection and post-processing.
    file(GLOB _atlas_tracy_viewer_exes "${_atlas_tracy_viewer_dir}/tracy-*.exe")
    add_custom_target(atlas_tracy_viewer ALL
      COMMAND ${CMAKE_COMMAND} -E make_directory "${ATLAS_BIN_ROOT}/tools"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${_atlas_tracy_viewer_exes} "${ATLAS_BIN_ROOT}/tools/"
      COMMENT "Deploying Tracy viewer + CLI tools to bin/<build>/tools/"
      VERBATIM
    )
    set_target_properties(atlas_tracy_viewer PROPERTIES FOLDER "ThirdParty/Tracy")
    unset(_atlas_tracy_viewer_url)
    unset(_atlas_tracy_viewer_archive)
    unset(_atlas_tracy_viewer_dir)
    unset(_atlas_tracy_viewer_marker)
    unset(_atlas_tracy_viewer_exes)
  else()
    message(WARNING
      "ATLAS_BUILD_TRACY_VIEWER on non-Windows: Tracy publishes no "
      "official Linux/macOS prebuilt. Build the viewer manually from "
      "${tracy_SOURCE_DIR}/profiler — see docs/operations/tracy_usage.md.")
  endif()
endif()

# ── Threading ────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Source tree ──────────────────────────────────────────────────────────────
add_subdirectory(src/lib)
add_subdirectory(src/server)
add_subdirectory(src/client)
add_subdirectory(src/tools)

if(ATLAS_BUILD_CSHARP)
  add_subdirectory(src/csharp)
  add_subdirectory(samples/stress)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(ATLAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ── IDE organization (must come after all targets are defined) ──────────────
include(AtlasFolders)
