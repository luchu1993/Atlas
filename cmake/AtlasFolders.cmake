# AtlasFolders.cmake
#
# Organize targets into Visual Studio solution folders and add header files
# to targets for IDE browsing. Include this AFTER all targets are defined.

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Tuck CMake's auto-generated targets (ZERO_CHECK, ALL_BUILD, INSTALL, …) under
# a dot-prefixed folder so IDEs sort/hide it out of the way.
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER ".cmake")

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
  atlas_space
  atlas_db
  atlas_db_sqlite
  atlas_db_xml
  atlas_db_mysql
  atlas_clrscript
  atlas_engine
)

atlas_set_folder("Lib"
  atlas_platform_config
  atlas_server_types
  atlas_db_iface
  ${_atlas_lib_targets}
)

atlas_add_ide_headers(${_atlas_lib_targets})

# ── App ─────────────────────────────────────────────────────────────────────
# Grouped by service; each sub-folder bundles the executable with its *_lib
# (and any service-local helper libs such as atlas_cellappmgr_bsp).
set(_atlas_app_base_targets     atlas_baseapp     atlas_baseapp_lib
                                atlas_baseappmgr  atlas_baseappmgr_lib)
set(_atlas_app_cell_targets     atlas_cellapp     atlas_cellapp_lib
                                atlas_cellappmgr  atlas_cellappmgr_lib
                                atlas_cellappmgr_bsp)
set(_atlas_app_db_targets       atlas_dbapp       atlas_dbapp_lib)
set(_atlas_app_login_targets    atlas_loginapp    atlas_loginapp_lib)
set(_atlas_app_machined_targets machined          atlas_machined_lib)
set(_atlas_app_echo_targets     atlas_echoapp     atlas_echoapp_lib)
set(_atlas_app_client_targets   atlas_client      atlas_client_lib)
set(_atlas_app_tool_targets     atlas_tool)

atlas_set_folder("App/base"     ${_atlas_app_base_targets})
atlas_set_folder("App/cell"     ${_atlas_app_cell_targets})
atlas_set_folder("App/db"       ${_atlas_app_db_targets})
atlas_set_folder("App/login"    ${_atlas_app_login_targets})
atlas_set_folder("App/machined" ${_atlas_app_machined_targets})
atlas_set_folder("App/echo"     ${_atlas_app_echo_targets})
atlas_set_folder("App/client"   ${_atlas_app_client_targets})
atlas_set_folder("App/tool"     ${_atlas_app_tool_targets})

atlas_add_ide_headers(
  ${_atlas_app_base_targets}
  ${_atlas_app_cell_targets}
  ${_atlas_app_db_targets}
  ${_atlas_app_login_targets}
  ${_atlas_app_machined_targets}
  ${_atlas_app_echo_targets}
  ${_atlas_app_client_targets}
  ${_atlas_app_tool_targets}
)

# ── CSharp ──────────────────────────────────────────────────────────────────
atlas_set_folder("CSharp"
  atlas_shared_dll
  atlas_clrhost_dll
  atlas_runtime_dll
  atlas_client_dll
)

atlas_set_folder("CSharp/Generators"
  atlas_generators_events_dll
  atlas_generators_def_dll
)

atlas_set_folder("CSharp/Test"
  atlas_smoke_test_dll
  atlas_runtime_test_dll
  atlas_stress_test_base_dll
  atlas_stress_test_cell_dll
)

# ── Test ────────────────────────────────────────────────────────────────────
# Unit and integration test targets are assigned their folder in
# atlas_add_test() (see tests/CMakeLists.txt).

set(_atlas_stress_targets
  login_stress
  world_stress
)

atlas_set_folder("Test/Stress" ${_atlas_stress_targets})
atlas_add_ide_headers(${_atlas_stress_targets})
