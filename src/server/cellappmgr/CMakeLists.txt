# cellappmgr — cell manager (Phase 11)
#
# PR-1 lands the message INTERFACE lib; the process executable + static lib
# follow in PR-5 (src/server/cellappmgr/{main.cc, cellappmgr.{h,cc}, bsp_tree.{h,cc}}).

# Header-only message definitions.
# Exposed as INTERFACE so both atlas_cellapp_lib (which needs to handle
# AddCellToSpace / UpdateGeometry / ShouldOffload inbound) and the future
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
