# atlas_foundation — core utilities (logging, memory, containers, time)

add_library(atlas_foundation STATIC
  clock.cc
  error.cc
  log.cc
  log_sinks.cc
  memory_tracker.cc
  pool_allocator.cc
  runtime.cc
  string_utils.cc
  timer_queue.cc
)

target_include_directories(atlas_foundation PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_foundation
  PUBLIC atlas_platform_config atlas_compiler_options
)

if(UNIX)
  target_link_libraries(atlas_foundation PUBLIC Threads::Threads)
endif()
