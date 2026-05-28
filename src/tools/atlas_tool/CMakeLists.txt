add_executable(atlas_tool main.cc)
target_link_libraries(atlas_tool PRIVATE
  atlas_foundation
  atlas_network
  atlas_physics
  atlas_server
  atlas_compiler_options
)
if(ATLAS_ENABLE_JOLT)
  target_link_libraries(atlas_tool PRIVATE atlas_physics_jolt)
  target_compile_definitions(atlas_tool PRIVATE ATLAS_ATLAS_TOOL_HAS_JOLT)
endif()
