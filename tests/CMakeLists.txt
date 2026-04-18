# tests/CMakeLists.txt

# Helper function to reduce test boilerplate
function(atlas_add_test)
  cmake_parse_arguments(ARG "" "NAME;LABEL" "SOURCES;DEPS" ${ARGN})

  if(NOT ARG_LABEL)
    set(ARG_LABEL "unit")
  endif()

  add_executable(${ARG_NAME} ${ARG_SOURCES})
  target_link_libraries(${ARG_NAME} PRIVATE GTest::gtest_main ${ARG_DEPS})
  target_include_directories(${ARG_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME})
  set_tests_properties(${ARG_NAME} PROPERTIES LABELS "${ARG_LABEL}")
endfunction()

add_subdirectory(unit)
add_subdirectory(integration)

if(ATLAS_BUILD_CSHARP)
  add_subdirectory(csharp)
endif()
