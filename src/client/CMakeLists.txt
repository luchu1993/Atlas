# atlas_client — console client application with embedded CLR

add_library(atlas_client_lib STATIC
  client_app.cc
  client_native_provider.cc
)

target_include_directories(atlas_client_lib PUBLIC "${CMAKE_SOURCE_DIR}/src/client")
target_link_libraries(atlas_client_lib
  PUBLIC
    atlas_clrscript
    atlas_db
    atlas_entitydef
    atlas_foundation
    atlas_network
    atlas_platform
    atlas_serialization
    atlas_server
    # Cross-server message headers
    atlas_baseapp_lib
    atlas_loginapp_lib
    atlas_compiler_options
)

add_executable(atlas_client main.cc)
target_link_libraries(atlas_client PRIVATE atlas_client_lib)
