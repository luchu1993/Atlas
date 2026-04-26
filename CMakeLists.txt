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
