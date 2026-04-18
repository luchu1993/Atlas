# EchoApp — simple echo/test server

add_library(atlas_echoapp_lib STATIC
  echo_app.cc
)

target_include_directories(atlas_echoapp_lib PUBLIC
  "${CMAKE_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(atlas_echoapp_lib
  PUBLIC atlas_server atlas_compiler_options
)

add_executable(atlas_echoapp main.cc)
target_link_libraries(atlas_echoapp PRIVATE atlas_echoapp_lib)
atlas_deploy_clr_runtime(atlas_echoapp)
