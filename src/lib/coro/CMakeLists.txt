# atlas_coro — coroutine-based async primitives

add_library(atlas_coro STATIC
  pending_rpc_registry.cc
)

target_include_directories(atlas_coro PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_coro
  PUBLIC atlas_foundation atlas_network atlas_compiler_options
)
