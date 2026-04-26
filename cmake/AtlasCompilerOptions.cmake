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

# ── Profiler gate ────────────────────────────────────────────────────────────
# Atlas-owned switch for the profiler abstraction in
# src/lib/foundation/profiler.h. When 0, every ATLAS_PROFILE_* macro
# expands to a no-op at the preprocessor stage — no Tracy symbol, no
# hidden runtime cost. Tracy's own TRACY_ENABLE is plumbed in lockstep
# from cmake/Dependencies.cmake so the two switches never diverge.
if(ATLAS_ENABLE_PROFILER)
  target_compile_definitions(atlas_compiler_options INTERFACE ATLAS_PROFILE_ENABLED=1)
else()
  target_compile_definitions(atlas_compiler_options INTERFACE ATLAS_PROFILE_ENABLED=0)
endif()

# ── Heap allocator backend ───────────────────────────────────────────────────
# Each supported value of ATLAS_HEAP_ALLOCATOR emits a distinct
# compile-time define so heap.cc can select the matching RawAlloc /
# RawFree implementation. Call sites only see foundation/heap.h, which
# is backend-agnostic — adding a new allocator means a new define here
# and a matching #elif branch in heap.cc, nothing in the rest of the
# codebase moves.
if(ATLAS_HEAP_ALLOCATOR STREQUAL "std")
  target_compile_definitions(atlas_compiler_options INTERFACE ATLAS_HEAP_STD=1)
elseif(ATLAS_HEAP_ALLOCATOR STREQUAL "mimalloc")
  target_compile_definitions(atlas_compiler_options INTERFACE ATLAS_HEAP_MIMALLOC=1)
endif()
