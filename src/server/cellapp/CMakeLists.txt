add_library(atlas_cellapp_lib STATIC
  aoi_trigger.cc
  cell.cc
  cell_entity.cc
  cell_movement_metrics.cc
  cell_movement_system.cc
  cellapp.cc
  cellapp_config.cc
  cellapp_native_provider.cc
  controller_codec.cc
  ghost_maintainer.cc
  movement_command_store.cc
  movement_input_buffer.cc
  movement_input_rate_limiter.cc
  movement_position_history_store.cc
  movement_state_store.cc
  offload_checker.cc
  real_entity_data.cc
  space.cc
  space_data.cc
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
  atlas_movement_sim
  atlas_server
  atlas_space
  atlas_clrscript
  atlas_baseapp_lib
  atlas_cellappmgr_messages
  atlas_cellappmgr_bsp
)
if(ATLAS_ENABLE_JOLT)
  target_link_libraries(atlas_cellapp_lib PUBLIC atlas_physics_jolt)
  target_compile_definitions(atlas_cellapp_lib PUBLIC ATLAS_CELLAPP_HAS_JOLT)
endif()

# The CellApp executable.
add_executable(atlas_cellapp main.cc)
target_link_libraries(atlas_cellapp PRIVATE atlas_cellapp_lib)
atlas_deploy_clr_runtime(atlas_cellapp)
