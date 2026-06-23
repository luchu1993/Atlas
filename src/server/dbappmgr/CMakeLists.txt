add_library(atlas_dbappmgr_lib STATIC
  dbappmgr.cc
)

target_include_directories(atlas_dbappmgr_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(atlas_dbappmgr_lib
  PUBLIC
    atlas_foundation
    atlas_network
    atlas_serialization
    atlas_server
    atlas_compiler_options
)

add_executable(atlas_dbappmgr main.cc)
target_link_libraries(atlas_dbappmgr PRIVATE atlas_dbappmgr_lib)
atlas_deploy_clr_runtime(atlas_dbappmgr)
