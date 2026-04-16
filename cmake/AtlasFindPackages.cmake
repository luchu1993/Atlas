# AtlasFindPackages.cmake
# Locate required and optional third-party dependencies.

include(FetchContent)

# ── Threads (always required) ────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Google Test (for unit tests) ─────────────────────────────────────────────
if(ATLAS_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    # Prevent overriding parent project's compiler/linker settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    foreach(_t gtest gtest_main gmock gmock_main)
        if(TARGET ${_t})
            set_target_properties(${_t} PROPERTIES FOLDER "ThirdParty/googletest")
        endif()
    endforeach()
endif()

# ── pugixml (XML parsing) ───────────────────────────────────────────────────
FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG        v1.14
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pugixml)
if(TARGET pugixml)
    set_target_properties(pugixml PROPERTIES FOLDER "ThirdParty")
endif()
if(TARGET pugixml-static)
    set_target_properties(pugixml-static PROPERTIES FOLDER "ThirdParty")
endif()
foreach(_t Continuous Experimental Nightly NightlyMemoryCheck)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES FOLDER "_CMake")
    endif()
endforeach()

# ── rapidjson (JSON parsing, header-only) ────────────────────────────────────
# We only need the headers, not rapidjson's own CMake targets.
# rapidjson's CMakeLists.txt uses cmake_minimum_required(VERSION 2.8) which
# CMake 3.31+ rejects (CMP0097).  Bypass it with FetchContent_Populate so that
# add_subdirectory is never called on rapidjson's source tree.
set(RAPIDJSON_BUILD_DOC      OFF CACHE BOOL "" FORCE)
set(RAPIDJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RAPIDJSON_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    rapidjson
    GIT_REPOSITORY https://github.com/Tencent/rapidjson.git
    # v1.1.0 (2016) is outdated; master has decades of bug fixes and C++11 improvements.
    # Pin to a known-good commit for reproducible builds.
    GIT_TAG        ab1842a2dae061284c0a62dca1cc6d5e7e37e346
    GIT_SHALLOW    FALSE
)
FetchContent_GetProperties(rapidjson)
if(NOT rapidjson_POPULATED)
    FetchContent_Populate(rapidjson)
endif()
# rapidjson_SOURCE_DIR is now set; consumers use it via SYSTEM PRIVATE include.

# ── zlib (compression) ───────────────────────────────────────────────────────
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)
foreach(_t zlibstatic zlib minigzip minigzip64 example example64)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES FOLDER "ThirdParty/zlib")
    endif()
endforeach()

# Provide an INTERFACE target with correct include paths so consumers can
# simply link "atlas_zlib" without worrying about FetchContent internals.
if(NOT TARGET atlas_zlib)
    add_library(atlas_zlib INTERFACE)
    target_link_libraries(atlas_zlib INTERFACE zlibstatic)
    target_include_directories(atlas_zlib INTERFACE
        ${zlib_SOURCE_DIR}
        ${zlib_BINARY_DIR}
    )
    set_target_properties(atlas_zlib PROPERTIES FOLDER "ThirdParty/zlib")
endif()

# ── SQLite3 (database backend, amalgamation from sqlite.org) ─────────────────
FetchContent_Declare(
    sqlite3
    URL      https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip
    URL_HASH SHA3_256=0e4df2263ef8169f9396b9535d9654d8d64ae8eb1a339ba06c7b12a72e6fb020
)
FetchContent_MakeAvailable(sqlite3)

# The amalgamation has no CMakeLists.txt — build it ourselves as a static lib.
if(NOT TARGET sqlite3)
    add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
    target_compile_definitions(sqlite3 PRIVATE
        SQLITE_THREADSAFE=1
        SQLITE_ENABLE_FTS5
        SQLITE_ENABLE_JSON1
    )
    if(WIN32)
        target_compile_definitions(sqlite3 PRIVATE SQLITE_API=)
    endif()
    if(MSVC)
        target_compile_options(sqlite3 PRIVATE /W0)
    else()
        target_compile_options(sqlite3 PRIVATE -w)
    endif()
endif()
set_target_properties(sqlite3 PROPERTIES FOLDER "ThirdParty")

# ── .NET SDK (for CLR scripting) ─────────────────────────────────────────────
include(${CMAKE_SOURCE_DIR}/cmake/FindDotNet.cmake)

# Generate runtimeconfig.json into the binary directory, substituting the
# runtime version detected by FindDotNet.cmake (DOTNET_RUNTIME_VERSION /
# DOTNET_RUNTIME_TFM).  Test binaries load it from ATLAS_BINARY_DIR/runtime/.
configure_file(
    "${CMAKE_SOURCE_DIR}/runtime/atlas_server.runtimeconfig.json.in"
    "${CMAKE_BINARY_DIR}/runtime/atlas_server.runtimeconfig.json"
    @ONLY
)

# ── OpenSSL (optional at Phase 2) ────────────────────────────────────────────
# find_package(OpenSSL REQUIRED)

# ── MySQL client (optional at Phase 2) ───────────────────────────────────────
# find_package(PkgConfig)
# pkg_check_modules(MYSQL IMPORTED_TARGET mysqlclient)
