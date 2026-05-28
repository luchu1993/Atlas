add_library(atlas_physics STATIC
  collision_asset.cc
  physics_query.cc
)

target_include_directories(atlas_physics PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_physics
  PUBLIC atlas_math atlas_compiler_options
  PRIVATE atlas_platform rapidjson
)
