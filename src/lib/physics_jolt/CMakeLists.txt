add_library(atlas_physics_jolt STATIC
  jolt_init.cc
  jolt_physics_query.cc
)

target_include_directories(atlas_physics_jolt PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_physics_jolt
  PUBLIC atlas_physics atlas_compiler_options
)
