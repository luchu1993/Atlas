# login_stress — authentication-path stress tool (client ↔ LoginApp ↔ BaseApp).

add_executable(login_stress main.cc)
target_link_libraries(login_stress PRIVATE
  atlas_baseapp_lib
  atlas_loginapp_lib
  atlas_network
  atlas_foundation
  atlas_compiler_options
)
