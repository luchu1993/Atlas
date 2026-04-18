# machined — machine daemon, process registration, service discovery

add_library(atlas_machined_lib STATIC
  listener_manager.cc
  machined_app.cc
  process_registry.cc
  watcher_forwarder.cc
)

target_include_directories(atlas_machined_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(atlas_machined_lib
  PUBLIC atlas_server atlas_compiler_options
)

add_executable(machined main.cc)
target_link_libraries(machined PRIVATE atlas_machined_lib)
