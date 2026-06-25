option(ATLAS_BUILD_IL2CPP_PROBE "Build the IL2CPP callback spike DLL" OFF)

if(ATLAS_BUILD_IL2CPP_PROBE)
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_library(atlas_il2cpp_probe STATIC probe.cc)
  else()
    add_library(atlas_il2cpp_probe SHARED probe.cc)
  endif()

  set_target_properties(atlas_il2cpp_probe PROPERTIES
    OUTPUT_NAME "atlas_il2cpp_probe"
    POSITION_INDEPENDENT_CODE ON
  )

  if(UNIX)
    set_target_properties(atlas_il2cpp_probe PROPERTIES
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
    )
  endif()

  if(APPLE AND NOT IOS)
    set_target_properties(atlas_il2cpp_probe PROPERTIES
      SUFFIX ".bundle"
      PREFIX ""
    )
  endif()

  atlas_set_output_dir("" atlas_il2cpp_probe)
endif()
