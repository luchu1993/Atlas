add_library(atlas_reviver_lib STATIC
  reviver.cc
)

target_include_directories(atlas_reviver_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(atlas_reviver_lib
  PUBLIC
    atlas_foundation
    atlas_network
    atlas_platform
    atlas_server
    atlas_cellappmgr_messages
    atlas_compiler_options
)

add_executable(atlas_reviver main.cc)
target_link_libraries(atlas_reviver PRIVATE atlas_reviver_lib)
atlas_deploy_clr_runtime(atlas_reviver)
