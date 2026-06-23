function(atlas_dotnet_project)
  cmake_parse_arguments(
    ARG
    "DEPLOY;BUILD_ALL_TFMS"
    "NAME;PROJECT_FILE;ASSEMBLY_NAME;CONFIGURATION;TARGET_FRAMEWORK"
    "DEPENDS"
    ${ARGN}
  )

  if(NOT ARG_CONFIGURATION)
    set(ARG_CONFIGURATION "Release")
  endif()

  if(NOT ARG_TARGET_FRAMEWORK)
    if(NOT DOTNET_RUNTIME_TFM)
      message(FATAL_ERROR
        "atlas_dotnet_project(${ARG_NAME}): TARGET_FRAMEWORK not set and "
        "DOTNET_RUNTIME_TFM not detected; include FindDotNet first.")
    endif()
    set(ARG_TARGET_FRAMEWORK "${DOTNET_RUNTIME_TFM}")
  endif()

  set(_proj_path "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_PROJECT_FILE}")

  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    # VS generators include the .csproj natively; restore first so MSBuild
    # has assets before native targets depend on the C# project.
    if(DOTNET_EXECUTABLE)
      message(STATUS "Restoring NuGet packages for ${ARG_NAME}")
      execute_process(
        COMMAND "${DOTNET_EXECUTABLE}" restore "${_proj_path}" --nologo --verbosity quiet
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _restore_result
        OUTPUT_VARIABLE _restore_output
        ERROR_VARIABLE _restore_output
      )
      if(NOT _restore_result EQUAL 0)
        message(FATAL_ERROR
          "dotnet restore failed for ${_proj_path} (exit ${_restore_result}):\n${_restore_output}")
      endif()
    else()
      message(WARNING
        "DOTNET_EXECUTABLE not found; cannot restore ${_proj_path}. "
        "VS solution build may skip this C# project and its dependents.")
    endif()

    include_external_msproject(${ARG_NAME} "${_proj_path}")

    if(ARG_DEPENDS)
      add_dependencies(${ARG_NAME} ${ARG_DEPENDS})
    endif()

    get_filename_component(_proj_dir "${ARG_PROJECT_FILE}" DIRECTORY)
    set(_platform "${CMAKE_GENERATOR_PLATFORM}")
    if(NOT _platform)
      set(_platform "x64")
    endif()
    set(_vs_bin_dir "${CMAKE_CURRENT_SOURCE_DIR}/${_proj_dir}/bin/${_platform}")
    set(_output_dir "${_vs_bin_dir}/$<CONFIG>/${ARG_TARGET_FRAMEWORK}")
    set(_output_dll "${_output_dir}/${ARG_ASSEMBLY_NAME}")

    set_target_properties(${ARG_NAME} PROPERTIES
      DOTNET_OUTPUT_DIR "${_output_dir}"
      DOTNET_ASSEMBLY "${_output_dll}"
    )

    if(ARG_DEPLOY)
      set(_src_dll "${_output_dir}/${ARG_ASSEMBLY_NAME}")
      add_custom_target(${ARG_NAME}_deploy ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ATLAS_BIN_ROOT}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src_dll}" "${ATLAS_BIN_ROOT}/"
        COMMENT "Deploying ${ARG_ASSEMBLY_NAME}"
        VERBATIM
      )
      add_dependencies(${ARG_NAME}_deploy ${ARG_NAME})
    endif()
  else()
    # dotnet --output propagates OutputPath through ProjectReference targets.
    # Keep one CMake output dir so shared dependencies resolve consistently.
    set(_output_dir "${CMAKE_BINARY_DIR}/csharp")
    set(_output_dll "${_output_dir}/${ARG_ASSEMBLY_NAME}")

    file(GLOB_RECURSE _cs_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/*.cs"
    )
    list(FILTER _cs_sources EXCLUDE REGEX ".*/obj/.*")
    list(FILTER _cs_sources EXCLUDE REGEX ".*/bin/.*")

    set(_dotnet_dep_outputs)
    foreach(_dep IN LISTS ARG_DEPENDS)
      get_target_property(_dep_assembly "${_dep}" DOTNET_ASSEMBLY)
      if(_dep_assembly)
        list(APPEND _dotnet_dep_outputs "${_dep_assembly}")
      endif()
    endforeach()

    # BUILD_ALL_TFMS also seeds per-project ref outputs for ProjectReference users.
    # The deploy build stays pinned to TARGET_FRAMEWORK and the CMake output dir.
    if(ARG_BUILD_ALL_TFMS)
      add_custom_command(
        OUTPUT "${_output_dll}"
        COMMAND "${DOTNET_EXECUTABLE}" build "${_proj_path}"
                --configuration "${ARG_CONFIGURATION}"
                -p:Platform=AnyCPU
                --nologo -v quiet
        COMMAND "${DOTNET_EXECUTABLE}" build "${_proj_path}"
                --configuration "${ARG_CONFIGURATION}"
                --framework "${ARG_TARGET_FRAMEWORK}"
                --output "${_output_dir}"
                --no-dependencies
                -p:Platform=AnyCPU
                --nologo -v quiet
        DEPENDS ${_cs_sources} "${_proj_path}" ${_dotnet_dep_outputs}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Building C# project (all TFMs): ${ARG_NAME}"
        VERBATIM
      )
    else()
      # CMake orders dependencies; --no-dependencies avoids recursive obj races.
      # AnyCPU prevents vcvars64 Platform=x64 from leaking into MSBuild.
      add_custom_command(
        OUTPUT "${_output_dll}"
        COMMAND "${DOTNET_EXECUTABLE}" build "${_proj_path}"
                --configuration "${ARG_CONFIGURATION}"
                --framework "${ARG_TARGET_FRAMEWORK}"
                --output "${_output_dir}"
                --no-dependencies
                -p:Platform=AnyCPU
                --nologo -v quiet
        DEPENDS ${_cs_sources} "${_proj_path}" ${_dotnet_dep_outputs}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Building C# project: ${ARG_NAME}"
        VERBATIM
      )
    endif()

    add_custom_target(${ARG_NAME} ALL DEPENDS "${_output_dll}")

    if(ARG_DEPENDS)
      add_dependencies(${ARG_NAME} ${ARG_DEPENDS})
    endif()

    set_target_properties(${ARG_NAME} PROPERTIES
      DOTNET_OUTPUT_DIR "${_output_dir}"
      DOTNET_ASSEMBLY "${_output_dll}"
    )

    if(ARG_DEPLOY AND CMAKE_BUILD_TYPE)
      add_custom_command(TARGET ${ARG_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ATLAS_BIN_ROOT}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_output_dll}" "${ATLAS_BIN_ROOT}/"
        VERBATIM
      )
    endif()
  endif()
endfunction()
