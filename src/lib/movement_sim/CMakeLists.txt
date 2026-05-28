add_library(atlas_movement_sim STATIC
  movement_curve_store.cc
  movement_sim.cc
)

target_include_directories(atlas_movement_sim PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_movement_sim
  PUBLIC atlas_math atlas_physics atlas_platform_config atlas_compiler_options
)
