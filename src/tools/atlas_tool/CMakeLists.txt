# atlas_tool — CLI for machined queries and watcher probing.

add_executable(atlas_tool main.cc)
target_link_libraries(atlas_tool PRIVATE
  atlas_foundation
  atlas_network
  atlas_server
  atlas_compiler_options
)
