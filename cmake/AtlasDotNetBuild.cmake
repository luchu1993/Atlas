# AtlasDotNetBuild.cmake
#
# Helper function to build C# projects via `dotnet build` (non-VS generators)
# or include them as native .csproj projects (Visual Studio generators).

# atlas_dotnet_project(
#   NAME            <target_name>
#   PROJECT_FILE    <path_to_csproj>
#   ASSEMBLY_NAME   <output.dll>
#   CONFIGURATION   <Debug|Release>  (default: Release)
#   TARGET_FRAMEWORK <framework>     (default: net9.0)
#   DEPENDS         <target ...>     (other dotnet targets this depends on)
#   DEPLOY                            (copy the built assembly into
#                                      ${ATLAS_BIN_ROOT} alongside every
#                                      EXE; omit for build-time-only
#                                      assemblies like Roslyn analyzers)
# )
function(atlas_dotnet_project)
  cmake_parse_arguments(ARG "DEPLOY" "NAME;PROJECT_FILE;ASSEMBLY_NAME;CONFIGURATION;TARGET_FRAMEWORK" "DEPENDS" ${ARGN})

  if(NOT ARG_CONFIGURATION)
    set(ARG_CONFIGURATION "Release")
  endif()

  if(NOT ARG_TARGET_FRAMEWORK)
    set(ARG_TARGET_FRAMEWORK "net9.0")
  endif()

  # Resolve the .csproj file path relative to current source dir
  set(_proj_path "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_PROJECT_FILE}")

  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    # ── Visual Studio: include the .csproj natively ──────────────────────
    #
    # `include_external_msproject` does NOT invoke `dotnet restore`. If the
    # project has no `obj/project.assets.json`, MSBuild silently skips the
    # C# project during a solution build, which in turn skips any native
    # target that depends on it via `add_dependencies` — the dependent
    # .exe is never produced and `ctest` reports it as "Not Run". Run
    # restore at configure time to guarantee the restore artefacts exist
    # before VS builds this project.
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
        "DOTNET_EXECUTABLE not found — cannot restore ${_proj_path}. "
        "VS solution build may skip this C# project and its dependents.")
    endif()

    include_external_msproject(${ARG_NAME} "${_proj_path}")

    if(ARG_DEPENDS)
      add_dependencies(${ARG_NAME} ${ARG_DEPENDS})
    endif()

    # Compute the VS output path so tests/consumers can locate the DLL
    get_filename_component(_proj_dir "${ARG_PROJECT_FILE}" DIRECTORY)
    set(_platform "${CMAKE_GENERATOR_PLATFORM}")
    if(NOT _platform)
      set(_platform "x64")
    endif()
    set(_output_dir "${CMAKE_CURRENT_SOURCE_DIR}/${_proj_dir}/bin/${_platform}/$<CONFIG>/${ARG_TARGET_FRAMEWORK}")
    set(_output_dll "${_output_dir}/${ARG_ASSEMBLY_NAME}")

    set_target_properties(${ARG_NAME} PROPERTIES
      DOTNET_OUTPUT_DIR "${_output_dir}"
      DOTNET_ASSEMBLY "${_output_dll}"
    )

    # Deploy assembly to ${ATLAS_BIN_ROOT} (flat layout — see
    # cmake/AtlasFolders.cmake). include_external_msproject targets
    # don't support POST_BUILD, so we create a separate custom target
    # that copies the DLL after the build.
    if(ARG_DEPLOY)
      set(_src_dll "${CMAKE_CURRENT_SOURCE_DIR}/${_proj_dir}/bin/${_platform}/$<CONFIG>/${ARG_TARGET_FRAMEWORK}/${ARG_ASSEMBLY_NAME}")
      add_custom_target(${ARG_NAME}_deploy ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ATLAS_BIN_ROOT}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src_dll}" "${ATLAS_BIN_ROOT}/"
        COMMENT "Deploying ${ARG_ASSEMBLY_NAME}"
        VERBATIM
      )
      add_dependencies(${ARG_NAME}_deploy ${ARG_NAME})
    endif()
  else()
    # ── Non-VS (Ninja, Make, etc.): build via dotnet CLI ─────────────────
    set(_output_dir "${CMAKE_BINARY_DIR}/csharp/${ARG_NAME}")
    set(_output_dll "${_output_dir}/${ARG_ASSEMBLY_NAME}")

    # Glob C# sources for change detection
    file(GLOB_RECURSE _cs_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/*.cs"
    )
    list(FILTER _cs_sources EXCLUDE REGEX ".*/obj/.*")
    list(FILTER _cs_sources EXCLUDE REGEX ".*/bin/.*")

    add_custom_command(
      OUTPUT "${_output_dll}"
      COMMAND "${DOTNET_EXECUTABLE}" build "${_proj_path}"
              --configuration "${ARG_CONFIGURATION}"
              --output "${_output_dir}"
              --nologo -v quiet
      DEPENDS ${_cs_sources} "${_proj_path}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      COMMENT "Building C# project: ${ARG_NAME}"
      VERBATIM
    )

    add_custom_target(${ARG_NAME} ALL DEPENDS "${_output_dll}")

    if(ARG_DEPENDS)
      add_dependencies(${ARG_NAME} ${ARG_DEPENDS})
    endif()

    # Export the output directory as a target property for dependent targets
    set_target_properties(${ARG_NAME} PROPERTIES
      DOTNET_OUTPUT_DIR "${_output_dir}"
      DOTNET_ASSEMBLY "${_output_dll}"
    )

    # Deploy assembly to ${ATLAS_BIN_ROOT} (flat layout).
    # Single-config generators: deploy for CMAKE_BUILD_TYPE only.
    if(ARG_DEPLOY AND CMAKE_BUILD_TYPE)
      add_custom_command(TARGET ${ARG_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ATLAS_BIN_ROOT}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_output_dll}" "${ATLAS_BIN_ROOT}/"
        VERBATIM
      )
    endif()
  endif()
endfunction()
