# atlas_cellapp_lib — CellApp runtime (Phase 10)
#
# Step 10.4 promotes this from an interface (messages only) to a static
# library that owns Space and CellEntity. Later steps add Witness,
# CellAppNativeProvider, and the CellApp process shell.

add_library(atlas_cellapp_lib STATIC
  aoi_trigger.cc
  cell.cc
  cell_entity.cc
  cellapp.cc
  cellapp_native_provider.cc
  real_entity_data.cc
  space.cc
  witness.cc
)

target_include_directories(atlas_cellapp_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server/cellapp"
  "${CMAKE_SOURCE_DIR}/src/server"
)
target_link_libraries(atlas_cellapp_lib PUBLIC
  atlas_platform_config
  atlas_compiler_options
  atlas_network
  atlas_serialization
  atlas_math
  atlas_server
  atlas_space
  atlas_clrscript
  atlas_baseapp_lib
  atlas_cellappmgr_messages
)

# The CellApp executable.
add_executable(atlas_cellapp main.cc)
target_link_libraries(atlas_cellapp PRIVATE atlas_cellapp_lib)
atlas_deploy_clr_runtime(atlas_cellapp)
