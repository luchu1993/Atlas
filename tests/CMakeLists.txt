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

  # Windows: copy every transitively-linked runtime DLL (nethost.dll for
  # CLR-dependent tests, zlib1.dll for compression, etc.) next to the test
  # exe so the OS loader resolves them at process start. Without this,
  # missing DLLs trigger a modal STATUS_DLL_NOT_FOUND dialog that blocks CI
  # and local runs. Delegated to a script so the empty-DLL case is handled
  # cleanly (cmake -E copy_if_different rejects zero source args).
  if(WIN32)
    add_custom_command(TARGET ${ARG_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND}
              "-DTARGET_DLLS=$<TARGET_RUNTIME_DLLS:${ARG_NAME}>"
              "-DTARGET_DIR=$<TARGET_FILE_DIR:${ARG_NAME}>"
              -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDLLs.cmake"
      VERBATIM
    )
    # TARGET_RUNTIME_DLLS misses (a) atlas_engine.dll, which is loaded at
    # runtime via DynamicLibrary::Load rather than linked, and (b) nethost.dll
    # on some MSVC configurations when the IMPORTED SHARED target is reached
    # only transitively. Deploy both explicitly for CLR-dependent tests.
    atlas_deploy_clr_runtime_if_needed(${ARG_NAME})
    atlas_deploy_db_plugins_if_needed(${ARG_NAME})
  endif()

  add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME})
  set_tests_properties(${ARG_NAME} PROPERTIES LABELS "${ARG_LABEL}")

  # Output directory: ${ATLAS_BIN_ROOT}/test/ — derived from the build
  # directory name so a parallel build (e.g. build/mimalloc alongside
  # build/debug) keeps its test exes in its own bin/ tree rather than
  # overwriting the default's.
  foreach(_cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_cfg}" _CFG)
    set_target_properties(${ARG_NAME} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${ATLAS_BIN_ROOT}/test"
    )
  endforeach()

  # Organize test targets into solution folders by label
  if(ARG_LABEL STREQUAL "integration")
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "Test/Integration")
  else()
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "Test/Unit")
  endif()
endfunction()

add_subdirectory(unit)
add_subdirectory(integration)

if(ATLAS_BUILD_CSHARP)
  add_subdirectory(csharp)
endif()
