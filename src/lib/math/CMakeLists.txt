# atlas_math — Vector, Matrix, Quaternion

add_library(atlas_math STATIC
  matrix4.cc
  quaternion.cc
)

target_include_directories(atlas_math PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_math
  PUBLIC atlas_platform_config atlas_compiler_options
)
