# atlas_entitydef — entity type definitions, data types

add_library(atlas_entitydef STATIC
  entity_def_registry.cc
)

target_include_directories(atlas_entitydef PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_entitydef
  PUBLIC atlas_foundation atlas_serialization atlas_compiler_options
)
