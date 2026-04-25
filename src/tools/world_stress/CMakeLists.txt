# world_stress — full-cluster stress tool (client ↔ LoginApp ↔ BaseApp ↔ CellApp).

add_executable(world_stress
  main.cc
  client_event_tap.cc
  script_clients.cc
)
target_link_libraries(world_stress PRIVATE
  atlas_baseapp_lib
  atlas_loginapp_lib
  atlas_network
  atlas_foundation
  atlas_platform
  atlas_compiler_options
)
