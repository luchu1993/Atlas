# AtlasCompilerOptions.cmake
#
# INTERFACE library that carries project-wide compiler flags and definitions.
# All Atlas targets link against this via target_link_libraries.

add_library(atlas_compiler_options INTERFACE)

# ── Architecture ─────────────────────────────────────────────────────────────
target_compile_definitions(atlas_compiler_options INTERFACE ATLAS_ARCH_X64=1)

# ── Windows / MSVC ───────────────────────────────────────────────────────────
target_compile_options(atlas_compiler_options INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:
    /W4 /WX /permissive- /utf-8
    /Zc:__cplusplus /Zc:preprocessor
    /wd4100
  >
)
target_compile_definitions(atlas_compiler_options INTERFACE
  $<$<PLATFORM_ID:Windows>:ATLAS_PLATFORM_WINDOWS=1>
)

# ── Linux / GCC / Clang ─────────────────────────────────────────────────────
target_compile_options(atlas_compiler_options INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang>:
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter
    -Wno-stringop-overflow
    -Wconversion
    -Wnon-virtual-dtor
    -fPIC
  >
)
target_compile_definitions(atlas_compiler_options INTERFACE
  $<$<PLATFORM_ID:Linux>:ATLAS_PLATFORM_LINUX=1>
)

# ── Debug define ─────────────────────────────────────────────────────────────
target_compile_definitions(atlas_compiler_options INTERFACE
  $<$<CONFIG:Debug>:ATLAS_DEBUG=1>
)
