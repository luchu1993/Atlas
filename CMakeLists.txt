cmake_minimum_required(VERSION 3.28)
project(Atlas VERSION 0.1.0 LANGUAGES C CXX)

# ── C++ standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd / IDE integration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────────────────────────
option(ATLAS_BUILD_TESTS "Build unit and integration tests" ON)
option(ATLAS_BUILD_CSHARP "Build C# projects via dotnet" ON)
option(ATLAS_DB_MYSQL "Enable MySQL database backend" OFF)
option(ATLAS_USE_IOURING "Enable io_uring I/O poller (Linux only)" OFF)

# ── CMake modules ────────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(AtlasCompilerOptions)
include(Dependencies)
include(FindDotNet)
include(AtlasRuntimeDeploy)

if(ATLAS_BUILD_CSHARP)
  include(AtlasDotNetBuild)
endif()

# ── Threading ────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Source tree ──────────────────────────────────────────────────────────────
add_subdirectory(src/lib)
add_subdirectory(src/server)
add_subdirectory(src/client)

if(ATLAS_BUILD_CSHARP)
  add_subdirectory(src/csharp)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(ATLAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ── IDE organization (must come after all targets are defined) ──────────────
include(AtlasFolders)
