# AtlasFolders.cmake
#
# Organize targets into Visual Studio solution folders and add header files
# to targets for IDE browsing. Include this AFTER all targets are defined.

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ── Helpers ─────────────────────────────────────────────────────────────────

# Set FOLDER property only if the target exists.
function(atlas_set_folder folder)
  foreach(target IN LISTS ARGN)
    if(TARGET ${target})
      set_target_properties(${target} PROPERTIES FOLDER "${folder}")
    endif()
  endforeach()
endfunction()

# Add header files from the target's source directory so they appear in the
# IDE project tree. Only touches STATIC / SHARED / OBJECT / EXECUTABLE
# targets (skips INTERFACE / IMPORTED / UTILITY).
function(atlas_add_ide_headers)
  foreach(target IN LISTS ARGN)
    if(NOT TARGET ${target})
      continue()
    endif()
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY" OR
       _type STREQUAL "UTILITY")
      continue()
    endif()
    get_target_property(_src_dir ${target} SOURCE_DIR)
    file(GLOB _headers "${_src_dir}/*.h")
    if(_headers)
      target_sources(${target} PRIVATE ${_headers})
    endif()
  endforeach()
endfunction()

# ── ThirdParty ──────────────────────────────────────────────────────────────
atlas_set_folder("ThirdParty"
  gtest gtest_main
  pugixml-static pugixml
  rapidjson
  zlib zlibstatic
  sqlite3
)

# ── Lib ─────────────────────────────────────────────────────────────────────
set(_atlas_lib_targets
  atlas_platform
  atlas_foundation
  atlas_math
  atlas_script
  atlas_serialization
  atlas_entitydef
  atlas_network
  atlas_coro
  atlas_server
  atlas_db
  atlas_db_sqlite
  atlas_db_xml
  atlas_db_mysql
  atlas_clrscript
  atlas_engine
  atlas_client_lib
)

atlas_set_folder("Lib"
  atlas_platform_config
  atlas_server_types
  atlas_db_iface
  ${_atlas_lib_targets}
)

atlas_add_ide_headers(${_atlas_lib_targets})

# ── App ─────────────────────────────────────────────────────────────────────
set(_atlas_app_targets
  machined
  atlas_loginapp
  atlas_baseappmgr
  atlas_baseapp
  atlas_dbapp
  atlas_echoapp
  atlas_client
)

set(_atlas_app_lib_targets
  atlas_machined_lib
  atlas_loginapp_lib
  atlas_baseappmgr_lib
  atlas_baseapp_lib
  atlas_dbapp_lib
  atlas_echoapp_lib
)

atlas_set_folder("App" ${_atlas_app_targets})
atlas_set_folder("App/Lib" ${_atlas_app_lib_targets})

atlas_add_ide_headers(${_atlas_app_lib_targets})

# ── CSharp ──────────────────────────────────────────────────────────────────
atlas_set_folder("CSharp"
  atlas_shared_dll
  atlas_generators_events_dll
  atlas_runtime_dll
)

atlas_set_folder("CSharp/Test"
  atlas_smoke_test_dll
  atlas_runtime_test_dll
)

# ── Test ────────────────────────────────────────────────────────────────────
# Unit and integration test targets are assigned their folder in
# atlas_add_test() (see tests/CMakeLists.txt).
