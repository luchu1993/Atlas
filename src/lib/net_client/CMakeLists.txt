option(ATLAS_BUILD_NET_CLIENT "Build the Unity native client DLL" OFF)

if(NOT ATLAS_BUILD_NET_CLIENT)
  return()
endif()

add_library(atlas_net_client_core STATIC
  client_api.cc
  client_session.cc
)

# src/server is on the path for the header-only baseapp/loginapp message
# definitions; we do not link any server library.
target_include_directories(atlas_net_client_core
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
         "${CMAKE_SOURCE_DIR}/src/server"
)

target_link_libraries(atlas_net_client_core
  PUBLIC
    atlas_network
    atlas_foundation
    atlas_platform
    atlas_serialization_binary
    atlas_compiler_options
)

target_precompile_headers(atlas_net_client_core REUSE_FROM atlas_foundation)

add_library(atlas_net_client SHARED
  client_api.cc
  client_session.cc
)

target_include_directories(atlas_net_client
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
         "${CMAKE_SOURCE_DIR}/src/server"
)

target_compile_definitions(atlas_net_client
  PRIVATE  ATLAS_NET_CLIENT_EXPORTS ATLAS_NET_CLIENT_DLL
  INTERFACE ATLAS_NET_CLIENT_DLL
)

target_link_libraries(atlas_net_client
  PRIVATE
    atlas_network
    atlas_foundation
    atlas_platform
    atlas_serialization_binary
    atlas_compiler_options
)

target_precompile_headers(atlas_net_client REUSE_FROM atlas_foundation)

if(UNIX)
  set_target_properties(atlas_net_client PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
  target_link_options(atlas_net_client PRIVATE
    $<$<PLATFORM_ID:Linux>:-Wl,--exclude-libs,ALL>
    $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
  )
endif()

set_target_properties(atlas_net_client PROPERTIES
  OUTPUT_NAME "atlas_net_client"
)
if(APPLE AND NOT IOS)
  set_target_properties(atlas_net_client PROPERTIES
    SUFFIX ".bundle"
    PREFIX ""
  )
endif()

# iOS forbids third-party dylibs; Unity links the .a via __Internal.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  add_library(atlas_net_client_static STATIC
    client_api.cc
    client_session.cc
  )
  target_include_directories(atlas_net_client_static
    PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
           "${CMAKE_SOURCE_DIR}/src/server"
  )
  target_compile_definitions(atlas_net_client_static
    PRIVATE ATLAS_NET_CLIENT_EXPORTS
  )
  target_link_libraries(atlas_net_client_static
    PUBLIC atlas_network atlas_foundation atlas_platform
           atlas_serialization_binary atlas_compiler_options
  )
endif()

atlas_set_output_dir("" atlas_net_client)
