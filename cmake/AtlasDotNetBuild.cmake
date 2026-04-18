# AtlasDotNetBuild.cmake
#
# Helper function to build C# projects via `dotnet build` (non-VS generators)
# or include them as native .csproj projects (Visual Studio generators).

# atlas_dotnet_project(
#   NAME            <target_name>
#   PROJECT_FILE    <path_to_csproj>
#   ASSEMBLY_NAME   <output.dll>
#   CONFIGURATION   <Debug|Release>  (default: Release)
#   DEPENDS         <target ...>     (other dotnet targets this depends on)
# )
function(atlas_dotnet_project)
  cmake_parse_arguments(ARG "" "NAME;PROJECT_FILE;ASSEMBLY_NAME;CONFIGURATION" "DEPENDS" ${ARGN})

  if(NOT ARG_CONFIGURATION)
    set(ARG_CONFIGURATION "Release")
  endif()

  # Resolve the .csproj file path relative to current source dir
  set(_proj_path "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_PROJECT_FILE}")

  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    # ── Visual Studio: include the .csproj natively ──────────────────────
    include_external_msproject(${ARG_NAME} "${_proj_path}")

    if(ARG_DEPENDS)
      add_dependencies(${ARG_NAME} ${ARG_DEPENDS})
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
  endif()
endfunction()
