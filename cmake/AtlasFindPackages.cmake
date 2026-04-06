# AtlasFindPackages.cmake
# Locate required and optional third-party dependencies.

# ── Threads (always required) ────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Google Test (for unit tests) ─────────────────────────────────────────────
if(ATLAS_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    # Prevent overriding parent project's compiler/linker settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ── Python 3 (for scripting, optional at Phase 1) ───────────────────────────
# find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# ── OpenSSL (optional at Phase 1) ────────────────────────────────────────────
# find_package(OpenSSL REQUIRED)

# ── MySQL client (optional at Phase 1) ───────────────────────────────────────
# find_package(PkgConfig)
# pkg_check_modules(MYSQL IMPORTED_TARGET mysqlclient)
