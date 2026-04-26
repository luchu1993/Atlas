# AtlasOutputDirectory.cmake
#
# Directs build artifacts into categorised subdirectories under bin/.
#
# Layout:  bin/<build_dir_name>/<subdir>/
#   e.g.   bin/debug/server/atlas_cellapp.exe
#          bin/release/lib/atlas_foundation.lib
#          bin/mimalloc/lib/atlas_foundation.lib   (build dir = build/mimalloc)
#
# The leading directory is the BUILD DIRECTORY name (last component of
# CMAKE_BINARY_DIR), not the configuration name. For Atlas's standard
# presets (debug, release, hybrid, asan, …) the build directory and
# configuration name happen to coincide, so the layout looks unchanged.
# For ad-hoc build directories — e.g. a parallel mimalloc / std heap
# experiment in build/mimalloc alongside build/debug — each gets its
# own bin/<name>/ tree and the two never overwrite each other's
# atlas_foundation.lib.
#
# Trade-off: if the same build directory builds multiple configurations
# (rare with Atlas's preset flow, but possible under VS multi-config),
# they collide on filename. The Atlas convention is "one build dir,
# one config"; users who deliberately build Debug + Release in one
# tree must accept that artifacts overwrite.
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

# Compute the bin/ root once at module include time. The cache variable
# is exposed so other CMake modules (AtlasFolders, AtlasDotNetBuild,
# tests/CMakeLists.txt) reach the same path without re-deriving it.
get_filename_component(_atlas_build_dir_name "${CMAKE_BINARY_DIR}" NAME)
set(ATLAS_BIN_ROOT "${CMAKE_SOURCE_DIR}/bin/${_atlas_build_dir_name}"
    CACHE INTERNAL "Atlas binary output root, derived from build directory name")
unset(_atlas_build_dir_name)

function(atlas_set_output_dir subdir)
  set(_dir "${ATLAS_BIN_ROOT}/${subdir}")

  foreach(target IN LISTS ARGN)
    if(NOT TARGET ${target})
      continue()
    endif()
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
      continue()
    endif()
    # Set every per-config slot to the same path so the output lands
    # in bin/<build_dir>/<subdir>/ regardless of which configuration
    # the multi-config generator (VS, Ninja Multi-Config) is currently
    # building. CMake selects the right slot at build time.
    foreach(_cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
      string(TOUPPER "${_cfg}" _CFG)
      set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${_dir}"
        LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${_dir}"
        ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${_dir}"
      )
    endforeach()
  endforeach()
endfunction()
