# world_stress — full-cluster stress tool (client ↔ LoginApp ↔ BaseApp ↔ CellApp).
#
# P2.1: source is a fork of login_stress/main.cc with only the usage banner
# renamed. Cell-ready / AoI / RPC instrumentation arrives in P2.2+.

add_executable(world_stress main.cc)
target_link_libraries(world_stress PRIVATE
  atlas_baseapp_lib
  atlas_loginapp_lib
  atlas_network
  atlas_foundation
  atlas_compiler_options
)
