# atlas_platform_config — header-only platform detection
add_library(atlas_platform_config INTERFACE)
target_include_directories(atlas_platform_config INTERFACE "${CMAKE_SOURCE_DIR}/src/lib")

# atlas_platform — OS abstraction layer
add_library(atlas_platform STATIC
  filesystem.cc
  io_poller.cc
  io_poller_select.cc
  signal_handler.cc
  threading.cc
)

if(WIN32)
  target_sources(atlas_platform PRIVATE
    child_process_windows.cc
    dynamic_library_windows.cc
    io_poller_wsapoll.cc
    threading_windows.cc
  )
elseif(UNIX)
  target_sources(atlas_platform PRIVATE
    child_process_linux.cc
    dynamic_library_linux.cc
    io_poller_epoll.cc
    threading_linux.cc
  )
endif()

if(ATLAS_USE_IOURING AND UNIX)
  target_sources(atlas_platform PRIVATE io_poller_io_uring.cc)
endif()

target_include_directories(atlas_platform PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_platform
  PUBLIC atlas_foundation atlas_compiler_options
)

if(WIN32)
  target_link_libraries(atlas_platform PUBLIC ws2_32)
elseif(UNIX)
  target_link_libraries(atlas_platform PUBLIC dl Threads::Threads)
endif()
