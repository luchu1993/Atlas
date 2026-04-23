# atlas_space — spatial index primitives (RangeList, RangeTrigger, Controllers)

add_library(atlas_space STATIC
  range_list.cc
  range_trigger.cc
  controllers.cc
  move_controller.cc
  timer_controller.cc
  proximity_controller.cc
)

target_include_directories(atlas_space PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_space
  PUBLIC atlas_platform_config atlas_compiler_options atlas_foundation atlas_math
)
