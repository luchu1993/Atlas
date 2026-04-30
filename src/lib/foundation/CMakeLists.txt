# atlas_foundation — core utilities (logging, memory, containers, time)

# heap.cc lives in its own OBJECT library so its `operator new` /
# `operator delete` overrides are force-linked into every binary that
# consumes atlas_foundation. A regular STATIC archive only pulls in
# object files whose symbols are referenced; replaceable allocation
# functions are not symbols the linker explicitly searches for, so they
# would silently fail to apply in any binary that doesn't otherwise
# touch heap.cc. OBJECT libraries propagate their .obj files into every
# consumer's link command directly, sidestepping that problem and
# guaranteeing one heap policy across atlas_engine.dll, every plugin
# DLL, and every test exe.
add_library(atlas_heap OBJECT heap.cc)
target_include_directories(atlas_heap PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_heap PUBLIC atlas_compiler_options)

# Tracy is a dependency only when the profiler is enabled. With it off,
# foundation/profiler.h's macros expand to no-ops, profiler.h doesn't
# include any Tracy header, and heap.cc never references a Tracy
# symbol — the link is genuinely unused, not just inert.
if(ATLAS_ENABLE_PROFILER)
  target_link_libraries(atlas_heap PUBLIC Tracy::TracyClient)
endif()

# Heap backend link — only when the mimalloc backend is selected. The
# default "std" path uses platform aligned-alloc / aligned-free and
# needs no extra link. mimalloc is propagated PUBLIC so its DLL is
# also a runtime dependency of every consumer, which is what
# atlas_deploy_runtime / TARGET_RUNTIME_DLLS picks up to copy the
# .dll alongside test exes.
if(ATLAS_HEAP_ALLOCATOR STREQUAL "mimalloc")
  target_link_libraries(atlas_heap PUBLIC Mimalloc::Mimalloc)
endif()

add_library(atlas_foundation STATIC
  clock.cc
  error.cc
  log.cc
  log_sinks.cc
  memory_tracker.cc
  pool_allocator.cc
  profiler.cc
  runtime.cc
  string_utils.cc
  timer_queue.cc
)

target_include_directories(atlas_foundation PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
# atlas_heap PUBLIC link propagates two things downstream:
#   1. heap.obj — included via OBJECT library .obj propagation, so the
#      operator new/delete overrides reach every binary regardless of
#      whether any of its TUs reference HeapAlloc.
#   2. The Tracy include path and link (when ATLAS_ENABLE_PROFILER is
#      on), so any TU in atlas_foundation that includes profiler.h
#      compiles. Single source of truth — no duplicate Tracy link
#      directly on atlas_foundation.
target_link_libraries(atlas_foundation
  PUBLIC atlas_platform_config atlas_compiler_options atlas_heap
)

if(UNIX)
  target_link_libraries(atlas_foundation PUBLIC Threads::Threads)
endif()

# Precompiled header — built once for atlas_foundation, then REUSE_FROM'd by
# the largest downstream libs (network/server/platform/clrscript). PRIVATE so
# the PCH itself isn't transitively forced into other targets that opt out.
target_precompile_headers(atlas_foundation PRIVATE atlas_common.h)
