# AtlasCompilerOptions.cmake
# Compiler and linker options for Atlas Engine

# ── Warning flags ────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus      # Report correct __cplusplus value
        /Zc:preprocessor     # Use conforming preprocessor
        /wd4100              # Unreferenced formal parameter (noisy in interfaces)
    )
    # Disable some overly strict warnings for third-party integration
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATE_NO_WARNINGS
        NOMINMAX                # Prevent min/max macros from windows.h
        WIN32_LEAN_AND_MEAN     # Reduce windows.h bloat
    )
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        -Wconversion
        -Wnon-virtual-dtor
    )
endif()

# ── Per-config flags ─────────────────────────────────────────────────────────
if(MSVC)
    # Debug: full debug info
    string(APPEND CMAKE_CXX_FLAGS_DEBUG          " /Zi /Od /DATLAS_DEBUG=1")
    # Release: full optimization
    string(APPEND CMAKE_CXX_FLAGS_RELEASE        " /O2 /DNDEBUG")
    # Hybrid (RelWithDebInfo): optimized + debug symbols
    string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " /O2 /Zi /DATLAS_DEBUG=0")
else()
    string(APPEND CMAKE_CXX_FLAGS_DEBUG          " -g -O0 -DATLAS_DEBUG=1")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE        " -O3 -DNDEBUG")
    string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " -O2 -g -DATLAS_DEBUG=0")
endif()

# ── Sanitizers ───────────────────────────────────────────────────────────────
if(NOT MSVC)
    if(ATLAS_ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()

    if(ATLAS_ENABLE_TSAN)
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    endif()

    if(ATLAS_ENABLE_UBSAN)
        add_compile_options(-fsanitize=undefined)
        add_link_options(-fsanitize=undefined)
    endif()
endif()

# ── Platform defines ─────────────────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(ATLAS_PLATFORM_WINDOWS=1)
elseif(UNIX AND NOT APPLE)
    add_compile_definitions(ATLAS_PLATFORM_LINUX=1)
elseif(APPLE)
    add_compile_definitions(ATLAS_PLATFORM_MACOS=1)
endif()

# Architecture
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(ATLAS_ARCH_X64=1)
else()
    add_compile_definitions(ATLAS_ARCH_X86=1)
endif()
