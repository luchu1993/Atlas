add_library(atlas_navigation STATIC
  nav_input.cc
  nav_params.cc
  nav_query.cc
)

target_include_directories(atlas_navigation PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_navigation
  PUBLIC atlas_math atlas_physics atlas_compiler_options
  PRIVATE atlas_platform rapidjson
)
