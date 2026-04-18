# baseapp — entity behavior, state, client proxy

add_library(atlas_baseapp_lib STATIC
  base_entity.cc
  baseapp.cc
  baseapp_native_provider.cc
  delta_forwarder.cc
  entity_manager.cc
  id_client.cc
)

target_include_directories(atlas_baseapp_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(atlas_baseapp_lib
  PUBLIC
    atlas_clrscript
    atlas_db
    atlas_entitydef
    atlas_foundation
    atlas_network
    atlas_server
    # Cross-server message headers
    atlas_baseappmgr_lib
    atlas_dbapp_lib
    atlas_loginapp_lib
    atlas_compiler_options
)

add_executable(atlas_baseapp main.cc)
target_link_libraries(atlas_baseapp PRIVATE atlas_baseapp_lib)
