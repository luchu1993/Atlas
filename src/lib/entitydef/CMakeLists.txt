# atlas_entitydef — entity type definitions, data types

add_library(atlas_entitydef STATIC
  entity_def_registry.cc
)

target_include_directories(atlas_entitydef PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_entitydef
  PUBLIC atlas_foundation atlas_serialization atlas_compiler_options
)

# atlas_entitydef_client — C ABI SHARED DLL exposing EntityDefRegistry to
# non-Atlas processes (UE plugin, future external clients). Gated on the
# same flag as atlas_net_client; the two ship together as the client SDK.
option(ATLAS_BUILD_ENTITYDEF_CLIENT
       "Build the entity-def client SDK DLL alongside atlas_net_client"
       ${ATLAS_BUILD_NET_CLIENT})

if(ATLAS_BUILD_ENTITYDEF_CLIENT)
  add_library(atlas_entitydef_client SHARED
    entitydef_api.cc
  )

  target_include_directories(atlas_entitydef_client
    PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
  )

  target_compile_definitions(atlas_entitydef_client
    PRIVATE  ATLAS_EDR_EXPORTS ATLAS_EDR_DLL
    INTERFACE ATLAS_EDR_DLL
  )

  target_link_libraries(atlas_entitydef_client
    PRIVATE atlas_entitydef
  )

  if(UNIX)
    set_target_properties(atlas_entitydef_client PROPERTIES
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
    )
    target_link_options(atlas_entitydef_client PRIVATE
      $<$<PLATFORM_ID:Linux>:-Wl,--exclude-libs,ALL>
      $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
    )
  endif()

  set_target_properties(atlas_entitydef_client PROPERTIES
    OUTPUT_NAME "atlas_entitydef_client"
  )

  atlas_set_output_dir("" atlas_entitydef_client)
endif()
