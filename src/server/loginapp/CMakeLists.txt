# loginapp — client authentication and connection

add_library(atlas_loginapp_lib STATIC
  loginapp.cc
)

target_include_directories(atlas_loginapp_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(atlas_loginapp_lib
  PUBLIC
    atlas_coro
    atlas_db_iface
    atlas_foundation
    atlas_network
    atlas_server
    atlas_compiler_options
)

add_executable(atlas_loginapp main.cc)
target_link_libraries(atlas_loginapp PRIVATE atlas_loginapp_lib)
atlas_deploy_clr_runtime(atlas_loginapp)
