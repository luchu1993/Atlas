# tests/integration/CMakeLists.txt — integration tests

atlas_add_test(NAME test_tcp_echo_roundtrip
  SOURCES test_tcp_echo_roundtrip.cpp
  DEPS atlas_network
  LABEL integration
)

atlas_add_test(NAME test_machined_registration
  SOURCES test_machined_registration.cpp
  DEPS atlas_machined_lib atlas_server atlas_network
  LABEL integration
)

atlas_add_test(NAME test_baseappmgr_registration
  SOURCES test_baseappmgr_registration.cpp
  DEPS atlas_baseappmgr_lib atlas_server atlas_network
  LABEL integration
)

atlas_add_test(NAME test_dbapp_login_flow
  SOURCES test_dbapp_login_flow.cpp
  DEPS atlas_dbapp_lib atlas_server atlas_network atlas_platform
  LABEL integration
)

atlas_add_test(NAME test_dbapp_checkout_cleanup
  SOURCES test_dbapp_checkout_cleanup.cpp
  DEPS atlas_dbapp_lib atlas_server atlas_network atlas_platform
  LABEL integration
)

atlas_add_test(NAME test_login_flow
  SOURCES test_login_flow.cpp
  DEPS atlas_baseapp_lib atlas_baseappmgr_lib atlas_dbapp_lib
       atlas_loginapp_lib atlas_machined_lib atlas_server
       atlas_network atlas_platform
  LABEL integration
)

if(ATLAS_BUILD_NET_CLIENT)
  # Shared FakeCluster for the C++ integration test and the C# IntegrationTests
  # P/Invoke surface (atlas_test_helpers below). Static so it can be linked
  # into both an executable and a shared library without ODR clashes.
  add_library(atlas_fake_cluster STATIC fake_cluster.cc)
  target_include_directories(atlas_fake_cluster PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(atlas_fake_cluster
    PUBLIC atlas_baseapp_lib atlas_loginapp_lib atlas_network atlas_platform
  )

  atlas_add_test(NAME test_client_flow
    SOURCES test_client_flow.cpp
    DEPS atlas_net_client_core atlas_fake_cluster
    LABEL integration
  )

  add_library(atlas_test_helpers SHARED fake_cluster_capi.cc)
  target_link_libraries(atlas_test_helpers PRIVATE atlas_fake_cluster)
  set_target_properties(atlas_test_helpers PROPERTIES
    POSITION_INDEPENDENT_CODE ON
  )
  atlas_set_output_dir("" atlas_test_helpers)
endif()

atlas_add_test(NAME test_rudp_two_network_interfaces
  SOURCES test_rudp_two_network_interfaces.cpp
  DEPS atlas_network
  LABEL integration
)

atlas_add_test(NAME test_cellapp_integration
  SOURCES test_cellapp_integration.cpp
  DEPS atlas_cellapp_lib atlas_server atlas_network
  LABEL integration
)

atlas_add_test(NAME test_cellappmgr_integration
  SOURCES test_cellappmgr_integration.cpp
  DEPS atlas_cellappmgr_lib atlas_cellappmgr_bsp atlas_cellapp_lib
       atlas_server atlas_network
  LABEL integration
)

atlas_add_test(NAME test_cellappmgr_process
  SOURCES test_cellappmgr_process.cpp
  DEPS atlas_cellappmgr_messages atlas_server atlas_network atlas_platform
  LABEL integration
)

atlas_add_test(NAME test_distributed_space
  SOURCES test_distributed_space.cpp
  DEPS atlas_cellapp_lib atlas_server atlas_network
  LABEL integration
)

atlas_add_test(NAME test_aoi_hysteresis_churn
  SOURCES test_aoi_hysteresis_churn.cpp
  DEPS atlas_cellapp_lib atlas_server atlas_network
  LABEL integration
)

atlas_add_test(NAME test_aoi_visibility
  SOURCES test_aoi_visibility.cpp
  DEPS atlas_cellapp_lib atlas_server atlas_network
  LABEL integration
)

atlas_add_test(NAME test_offload_traversal
  SOURCES test_offload_traversal.cpp
  DEPS atlas_cellapp_lib atlas_cellappmgr_bsp atlas_server atlas_network
  LABEL integration
)

# Hot reload: bootstraps real ClrScriptEngine + Atlas.Runtime, exercises
# ClrHotReload state machine + failure paths. End-to-end SerializeAndUnload
# round-trip is covered by C# ScriptHostTests.
add_executable(test_hot_reload
  test_hot_reload.cpp
)
target_link_libraries(test_hot_reload PRIVATE GTest::gtest_main atlas_clrscript atlas_platform)
target_include_directories(test_hot_reload PRIVATE "${CMAKE_SOURCE_DIR}/tests")
target_compile_definitions(test_hot_reload PRIVATE _CRT_SECURE_NO_WARNINGS)
add_dependencies(test_hot_reload atlas_engine atlas_runtime_dll)
if(TARGET atlas_runtime_dll_deploy)
  add_dependencies(test_hot_reload atlas_runtime_dll_deploy)
endif()
add_test(NAME test_hot_reload COMMAND test_hot_reload)
set_tests_properties(test_hot_reload PROPERTIES
  LABELS "integration"
  ENVIRONMENT
    "ATLAS_ENGINE_RLOC=$<TARGET_FILE:atlas_engine>;ATLAS_RUNTIME_CONFIG_RLOC=${CMAKE_BINARY_DIR}/runtime/atlas_server.runtimeconfig.json;ATLAS_RUNTIME_DLL_RLOC=${ATLAS_BIN_ROOT}/Atlas.Runtime.dll"
)
foreach(_cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${_cfg}" _CFG)
  set_target_properties(test_hot_reload PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${ATLAS_BIN_ROOT}"
  )
endforeach()
set_target_properties(test_hot_reload PROPERTIES FOLDER "Test/Integration")
if(WIN32)
  add_custom_command(TARGET test_hot_reload POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            "-DTARGET_DLLS=$<TARGET_RUNTIME_DLLS:test_hot_reload>"
            "-DTARGET_DIR=$<TARGET_FILE_DIR:test_hot_reload>"
            -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDLLs.cmake"
    VERBATIM
  )
  atlas_deploy_clr_runtime_if_needed(test_hot_reload)
  atlas_deploy_db_plugins_if_needed(test_hot_reload)
endif()
