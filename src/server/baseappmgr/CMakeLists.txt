# baseappmgr — cluster management for BaseApp instances

add_library(atlas_baseappmgr_lib STATIC
  baseappmgr.cc
)

target_include_directories(atlas_baseappmgr_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(atlas_baseappmgr_lib
  PUBLIC
    atlas_foundation
    atlas_network
    atlas_server
    atlas_loginapp_lib
    atlas_compiler_options
)

add_executable(atlas_baseappmgr main.cc)
target_link_libraries(atlas_baseappmgr PRIVATE atlas_baseappmgr_lib)
atlas_deploy_clr_runtime(atlas_baseappmgr)
