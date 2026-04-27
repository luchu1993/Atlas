# crash_demo — manual validator for the platform crash handler.

add_executable(crash_demo main.cc)
target_link_libraries(crash_demo PRIVATE
  atlas_platform
  atlas_compiler_options
)
