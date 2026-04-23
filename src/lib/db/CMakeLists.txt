# atlas_db_iface — header-only database interface
add_library(atlas_db_iface INTERFACE)
target_include_directories(atlas_db_iface INTERFACE "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_db_iface
  INTERFACE atlas_entitydef atlas_foundation atlas_network atlas_server_types
)

# atlas_db — database factory (loads backend plugins at runtime via DynamicLibrary)
add_library(atlas_db STATIC
  database_factory.cc
)

target_include_directories(atlas_db PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_db
  PUBLIC
    atlas_db_iface
    atlas_foundation
    atlas_network
    atlas_platform
    atlas_compiler_options
)
