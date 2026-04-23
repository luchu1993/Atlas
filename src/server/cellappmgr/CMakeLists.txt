# cellappmgr — cell manager.

# Header-only message definitions.
# Exposed as INTERFACE so both atlas_cellapp_lib (which needs to handle
# AddCellToSpace / UpdateGeometry / ShouldOffload inbound) and
# atlas_cellappmgr_lib can consume the same message header without creating
# a cellapp↔cellappmgr lib-level cycle.
add_library(atlas_cellappmgr_messages INTERFACE)
target_include_directories(atlas_cellappmgr_messages INTERFACE
  "${CMAKE_SOURCE_DIR}/src/server"
)
target_link_libraries(atlas_cellappmgr_messages INTERFACE
  atlas_network
  atlas_serialization
  atlas_server
)

# BSP tree — spatial partition shared by CellAppMgr (authoritative) and
# CellApp (deserialises UpdateGeometry blobs). Kept in its own STATIC lib
# so atlas_cellapp_lib can link it without pulling in the manager
# process code.
add_library(atlas_cellappmgr_bsp STATIC
  bsp_tree.cc
)
target_include_directories(atlas_cellappmgr_bsp PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
)
target_link_libraries(atlas_cellappmgr_bsp PUBLIC
  atlas_foundation
  atlas_network
  atlas_serialization
  atlas_server
  atlas_cellappmgr_messages
)

# CellAppMgr manager process library.
add_library(atlas_cellappmgr_lib STATIC
  cellappmgr.cc
)
target_include_directories(atlas_cellappmgr_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(atlas_cellappmgr_lib
  PUBLIC
    atlas_foundation
    atlas_network
    atlas_serialization
    atlas_server
    atlas_cellappmgr_messages
    atlas_cellappmgr_bsp
    atlas_compiler_options
)

add_executable(atlas_cellappmgr main.cc)
target_link_libraries(atlas_cellappmgr PRIVATE atlas_cellappmgr_lib)
atlas_deploy_clr_runtime(atlas_cellappmgr)
