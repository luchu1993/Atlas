# atlas_clrscript — .NET CLR embedding via hostfxr

add_library(atlas_clrscript STATIC
  base_native_provider.cc
  clr_bootstrap.cc
  clr_error.cc
  clr_host.cc
  clr_hot_reload.cc
  clr_marshal.cc
  clr_object.cc
  clr_object_registry.cc
  clr_script_engine.cc
  file_watcher.cc
  native_api_provider.cc
)

if(WIN32)
  target_sources(atlas_clrscript PRIVATE clr_host_windows.cc)
elseif(UNIX)
  target_sources(atlas_clrscript PRIVATE clr_host_linux.cc)
endif()

target_include_directories(atlas_clrscript PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_compile_definitions(atlas_clrscript PUBLIC ATLAS_ENGINE_EXPORTS)
target_link_libraries(atlas_clrscript
  PUBLIC
    atlas_entitydef
    atlas_foundation
    atlas_math
    atlas_platform
    atlas_script
    atlas_compiler_options
)

if(DOTNET_FOUND)
  target_link_libraries(atlas_clrscript PUBLIC DotNet::nethost)
endif()

# atlas_engine — shared library (.dll/.so) for C# P/Invoke
add_library(atlas_engine SHARED
  clr_native_api.cc
)

target_compile_definitions(atlas_engine PRIVATE ATLAS_ENGINE_EXPORTS)
target_link_libraries(atlas_engine
  PRIVATE atlas_clrscript atlas_foundation atlas_platform
)

if(UNIX)
  set_target_properties(atlas_engine PROPERTIES
    CXX_VISIBILITY_PRESET hidden
  )
endif()

# Output name: atlas_engine.dll on Windows, libatlas_engine.so on Linux
set_target_properties(atlas_engine PROPERTIES
  OUTPUT_NAME "atlas_engine"
)
