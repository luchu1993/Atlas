# atlas_server_types — header-only message/type definitions
add_library(atlas_server_types INTERFACE)
target_include_directories(atlas_server_types INTERFACE "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_server_types
  INTERFACE atlas_foundation atlas_network atlas_platform
)

# atlas_server — server framework base classes
add_library(atlas_server STATIC
  cellapp_peer_registry.cc
  entity_app.cc
  ipv4_networks.cc
  machined_client.cc
  manager_app.cc
  script_app.cc
  server_app.cc
  server_config.cc
  updatable.cc
  watcher.cc
)

target_include_directories(atlas_server PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_server
  PUBLIC
    atlas_server_types
    atlas_clrscript
    atlas_entitydef
    atlas_foundation
    atlas_network
    atlas_platform
    atlas_script
    atlas_serialization
    atlas_compiler_options
)

target_precompile_headers(atlas_server REUSE_FROM atlas_foundation)
