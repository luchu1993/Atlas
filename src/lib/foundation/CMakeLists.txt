# atlas_foundation — core utilities (logging, memory, containers, time)

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
target_link_libraries(atlas_foundation
  PUBLIC atlas_platform_config atlas_compiler_options
)

# Profiler backend: Tracy is propagated PUBLIC so any consumer of
# foundation/profiler.h transparently gets Tracy headers and link
# symbols when enabled. When the profiler is off, we link nothing —
# foundation/profiler.h's macros expand to no-ops and never reference
# any Tracy symbol, so the dependency simply does not exist.
if(ATLAS_ENABLE_PROFILER)
  target_link_libraries(atlas_foundation PUBLIC Tracy::TracyClient)
endif()

if(UNIX)
  target_link_libraries(atlas_foundation PUBLIC Threads::Threads)
endif()
