add_library(atlas_navigation_recast STATIC
  detour_nav_query.cc
  recast_bake.cc
  recast_nav_backend.cc
)

target_include_directories(atlas_navigation_recast PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_navigation_recast
  PUBLIC atlas_navigation atlas_compiler_options
  PRIVATE Recast Detour
)
