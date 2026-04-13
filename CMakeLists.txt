cmake_minimum_required(VERSION 3.20)

project(Atlas
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Atlas - Modern Distributed MMO Server Framework"
)

# ── C++20 ────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static libraries may be linked into shared libraries (e.g. atlas_engine.so),
# so compile everything with position-independent code on Linux.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Module path ──────────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ── Options ──────────────────────────────────────────────────────────────────
option(ATLAS_BUILD_TESTS      "Build unit tests"          ON)
option(ATLAS_BUILD_SERVER     "Build server applications" ON)
option(ATLAS_BUILD_CLIENT_SDK "Build client SDK"          ON)
option(ATLAS_ENABLE_ASAN      "Enable AddressSanitizer"   OFF)
option(ATLAS_ENABLE_TSAN      "Enable ThreadSanitizer"    OFF)
option(ATLAS_ENABLE_UBSAN     "Enable UBSanitizer"        OFF)

# ── Build configurations ─────────────────────────────────────────────────────
# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Custom "Hybrid" config (optimized + debug symbols)
set(CMAKE_C_FLAGS_HYBRID          "${CMAKE_C_FLAGS_RELWITHDEBINFO}"   CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_HYBRID        "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_HYBRID "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING "" FORCE)

# ── Output directories ───────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ── IDE folder grouping (Visual Studio solution folders) ─────────────────────
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Suppress ZERO_CHECK target (CMake re-runs on source changes are still
# triggered automatically by MSBuild via the .vcxproj dependencies).
set(CMAKE_SUPPRESS_REGENERATION ON)

# Sink ALL_BUILD and CTest dashboard targets into a hidden-looking folder.
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_CMake")
set_property(GLOBAL PROPERTY CTEST_TARGETS_FOLDER     "_CMake")

# ── CMake modules ────────────────────────────────────────────────────────────
include(AtlasCompilerOptions)
include(AtlasMacros)
include(AtlasFindPackages)

# ── Testing ──────────────────────────────────────────────────────────────────
if(ATLAS_BUILD_TESTS)
    enable_testing()
    include(CTest)
endif()

# ── Source tree ──────────────────────────────────────────────────────────────
add_subdirectory(src/lib)

if(ATLAS_BUILD_SERVER)
    add_subdirectory(src/server)
endif()

if(ATLAS_BUILD_CLIENT_SDK)
    add_subdirectory(src/client_sdk)
endif()

add_subdirectory(src/tools)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
