# dbapp — database persistence

add_library(atlas_dbapp_lib STATIC
  checkout_manager.cc
  dbapp.cc
  entity_id_allocator.cc
)

target_include_directories(atlas_dbapp_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(atlas_dbapp_lib
  PUBLIC
    atlas_db
    atlas_entitydef
    atlas_foundation
    atlas_network
    atlas_server
    atlas_loginapp_lib
    atlas_compiler_options
)

add_executable(atlas_dbapp main.cc)
target_link_libraries(atlas_dbapp PRIVATE atlas_dbapp_lib)
atlas_deploy_clr_runtime(atlas_dbapp)
atlas_deploy_db_plugins(atlas_dbapp)
