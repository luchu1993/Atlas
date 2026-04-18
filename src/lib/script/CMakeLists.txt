# atlas_script — language-agnostic scripting abstraction

add_library(atlas_script STATIC
  script_events.cc
  script_value.cc
)

target_include_directories(atlas_script PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_script
  PUBLIC atlas_foundation atlas_compiler_options
)
