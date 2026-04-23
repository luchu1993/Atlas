# AtlasOutputDirectory.cmake
#
# Directs build artifacts into categorised subdirectories under bin/.
#
# Layout:  bin/<config_snake_case>/<subdir>/
#   e.g.   bin/debug/server/atlas_cellapp.exe
#          bin/release/lib/atlas_foundation.lib
#
# Usage:
#   atlas_set_output_dir("server" atlas_cellapp atlas_baseapp ...)

# Convert a PascalCase config name to snake_case:
#   Debug → debug,  Release → release,
#   RelWithDebInfo → rel_with_deb_info,  MinSizeRel → min_size_rel
function(_atlas_config_to_snake config out_var)
  string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" _snake "${config}")
  string(TOLOWER "${_snake}" _snake)
  set(${out_var} "${_snake}" PARENT_SCOPE)
endfunction()

function(atlas_set_output_dir subdir)
  # Pre-compute snake_case directory names for each known configuration.
  set(_configs Debug Release RelWithDebInfo MinSizeRel)
  foreach(_cfg IN LISTS _configs)
    _atlas_config_to_snake("${_cfg}" _snake)
    string(TOUPPER "${_cfg}" _CFG)
    set(_dir_${_CFG} "${CMAKE_SOURCE_DIR}/bin/${_snake}/${subdir}")
  endforeach()

  foreach(target IN LISTS ARGN)
    if(NOT TARGET ${target})
      continue()
    endif()
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
      continue()
    endif()
    foreach(_cfg IN LISTS _configs)
      string(TOUPPER "${_cfg}" _CFG)
      set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${_dir_${_CFG}}"
        LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${_dir_${_CFG}}"
        ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${_dir_${_CFG}}"
      )
    endforeach()
  endforeach()
endfunction()
