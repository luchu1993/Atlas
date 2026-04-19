# tests/unit/CMakeLists.txt — all unit tests

# ── Basic / Foundation ──────────────────────────────────────────────────────

atlas_add_test(NAME test_hello SOURCES test_hello.cpp)

atlas_add_test(NAME test_platform_config
  SOURCES test_platform_config.cpp
  DEPS atlas_platform_config atlas_compiler_options
)

atlas_add_test(NAME test_error
  SOURCES test_error.cpp
  DEPS atlas_foundation
)

atlas_add_test(NAME test_string_utils
  SOURCES test_string_utils.cpp
  DEPS atlas_foundation
)

atlas_add_test(NAME test_log
  SOURCES test_log.cpp
  DEPS atlas_foundation
)

atlas_add_test(NAME test_time
  SOURCES test_time.cpp
  DEPS atlas_foundation
)

atlas_add_test(NAME test_timer_queue
  SOURCES test_timer_queue.cpp
  DEPS atlas_foundation
)

atlas_add_test(NAME test_intrusive_ptr
  SOURCES test_intrusive_ptr.cpp
  DEPS atlas_foundation
)

atlas_add_test(NAME test_pool_allocator
  SOURCES test_pool_allocator.cpp
  DEPS atlas_foundation
)

atlas_add_test(NAME test_containers
  SOURCES test_containers.cpp
  DEPS atlas_foundation
)

atlas_add_test(NAME test_log_sinks
  SOURCES test_log_sinks.cpp
  DEPS atlas_foundation
)

# ── Platform ────────────────────────────────────────────────────────────────

atlas_add_test(NAME test_filesystem
  SOURCES test_filesystem.cpp
  DEPS atlas_platform
)

atlas_add_test(NAME test_threading
  SOURCES test_threading.cpp
  DEPS atlas_platform
)

atlas_add_test(NAME test_signal_handler
  SOURCES test_signal_handler.cpp
  DEPS atlas_platform
)

atlas_add_test(NAME test_dynamic_library
  SOURCES test_dynamic_library.cpp
  DEPS atlas_platform
)

# ── Serialization ───────────────────────────────────────────────────────────

atlas_add_test(NAME test_binary_stream
  SOURCES test_binary_stream.cpp
  DEPS atlas_serialization
)

atlas_add_test(NAME test_data_section
  SOURCES test_data_section.cpp
  DEPS atlas_serialization
)

# ── Math ────────────────────────────────────────────────────────────────────

atlas_add_test(NAME test_math
  SOURCES test_math.cpp
  DEPS atlas_math
)

# ── Network ─────────────────────────────────────────────────────────────────

atlas_add_test(NAME test_address
  SOURCES test_address.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_socket
  SOURCES test_socket.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_event_dispatcher
  SOURCES test_event_dispatcher.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_message
  SOURCES test_message.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_bundle
  SOURCES test_bundle.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_channel
  SOURCES test_channel.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_network_interface
  SOURCES test_network_interface.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_bg_task_manager
  SOURCES test_bg_task_manager.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_seq_num
  SOURCES test_seq_num.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_reliable_udp
  SOURCES test_reliable_udp.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_compression_filter
  SOURCES test_compression_filter.cpp
  DEPS atlas_network
)

atlas_add_test(NAME test_machined_types
  SOURCES test_machined_types.cpp
  DEPS atlas_network atlas_server
)

# ── Script ──────────────────────────────────────────────────────────────────

atlas_add_test(NAME test_script_events
  SOURCES test_script_events.cpp
  DEPS atlas_script
)

atlas_add_test(NAME test_script_value
  SOURCES test_script_value.cpp
  DEPS atlas_script
)

# ── Entity Definition ──────────────────────────────────────────────────────

atlas_add_test(NAME test_entity_def_registry_json
  SOURCES test_entity_def_registry_json.cpp
  DEPS atlas_entitydef atlas_serialization
)

atlas_add_test(NAME test_entity_def_registry_rpc
  SOURCES test_entity_def_registry_rpc.cpp
  DEPS atlas_entitydef atlas_serialization
)

# ── Space (Phase 10: RangeList / RangeTrigger / Controller) ────────────────

atlas_add_test(NAME test_range_list
  SOURCES test_range_list.cpp
  DEPS atlas_space
)

atlas_add_test(NAME test_controllers
  SOURCES test_controllers.cpp
  DEPS atlas_space
)

atlas_add_test(NAME test_range_trigger
  SOURCES test_range_trigger.cpp
  DEPS atlas_space
)

atlas_add_test(NAME test_cellapp_messages
  SOURCES test_cellapp_messages.cpp
  DEPS atlas_cellapp_lib atlas_serialization
)

atlas_add_test(NAME test_intercell_messages
  SOURCES test_intercell_messages.cpp
  DEPS atlas_cellapp_lib atlas_serialization
)

atlas_add_test(NAME test_cellappmgr_messages
  SOURCES test_cellappmgr_messages.cpp
  DEPS atlas_cellappmgr_messages atlas_cellapp_lib atlas_serialization
)

atlas_add_test(NAME test_bsp_tree
  SOURCES test_bsp_tree.cpp
  DEPS atlas_cellappmgr_bsp atlas_serialization
)

atlas_add_test(NAME test_space
  SOURCES test_space.cpp
  DEPS atlas_cellapp_lib atlas_space
)

atlas_add_test(NAME test_cell_entity
  SOURCES test_cell_entity.cpp
  DEPS atlas_cellapp_lib atlas_space
)

atlas_add_test(NAME test_real_ghost
  SOURCES test_real_ghost.cpp
  DEPS atlas_cellapp_lib atlas_space atlas_network
)

atlas_add_test(NAME test_controller_codec
  SOURCES test_controller_codec.cpp
  DEPS atlas_cellapp_lib atlas_space atlas_serialization
)

atlas_add_test(NAME test_cell
  SOURCES test_cell.cpp
  DEPS atlas_cellapp_lib atlas_space
)

atlas_add_test(NAME test_ghost_maintainer
  SOURCES test_ghost_maintainer.cpp
  DEPS atlas_cellapp_lib atlas_cellappmgr_bsp atlas_space atlas_network
)

atlas_add_test(NAME test_offload_checker
  SOURCES test_offload_checker.cpp
  DEPS atlas_cellapp_lib atlas_cellappmgr_bsp atlas_space
)

atlas_add_test(NAME test_cellappmgr
  SOURCES test_cellappmgr.cpp
  DEPS atlas_cellappmgr_lib atlas_cellappmgr_bsp atlas_network
)

atlas_add_test(NAME test_witness_lifecycle
  SOURCES test_witness_lifecycle.cpp
  DEPS atlas_cellapp_lib atlas_space
)

atlas_add_test(NAME test_witness_replication
  SOURCES test_witness_replication.cpp
  DEPS atlas_cellapp_lib atlas_space
)

atlas_add_test(NAME test_witness_peer_lifetime
  SOURCES test_witness_peer_lifetime.cpp
  DEPS atlas_cellapp_lib atlas_space
)

atlas_add_test(NAME test_range_trigger_lifetime
  SOURCES test_range_trigger_lifetime.cpp
  DEPS atlas_cellapp_lib atlas_space
)

atlas_add_test(NAME test_cellapp_native_provider
  SOURCES test_cellapp_native_provider.cpp
  DEPS atlas_cellapp_lib atlas_space atlas_clrscript
)

atlas_add_test(NAME test_cellapp_handlers
  SOURCES test_cellapp_handlers.cpp
  DEPS atlas_cellapp_lib atlas_space atlas_entitydef
)

# ── Database ────────────────────────────────────────────────────────────────

atlas_add_test(NAME test_database_types
  SOURCES test_database_types.cpp
  DEPS atlas_db
)

atlas_add_test(NAME test_xml_database
  SOURCES test_xml_database.cpp
  DEPS atlas_db_xml atlas_entitydef
)

atlas_add_test(NAME test_sqlite_database
  SOURCES test_sqlite_database.cpp
  DEPS atlas_db_sqlite atlas_entitydef atlas_platform
)

# ── Coroutine ───────────────────────────────────────────────────────────────

atlas_add_test(NAME test_coro_task
  SOURCES test_coro_task.cpp
  DEPS atlas_coro
)

atlas_add_test(NAME test_coro_sleep
  SOURCES test_coro_sleep.cpp
  DEPS atlas_coro
)

atlas_add_test(NAME test_coro_rpc
  SOURCES test_coro_rpc.cpp
  DEPS atlas_coro
)

# ── Server Framework ───────────────────────────────────────────────────────

atlas_add_test(NAME test_watcher
  SOURCES test_watcher.cpp
  DEPS atlas_server
)

atlas_add_test(NAME test_updatable
  SOURCES test_updatable.cpp
  DEPS atlas_server
)

atlas_add_test(NAME test_server_config
  SOURCES test_server_config.cpp
  DEPS atlas_server
)

atlas_add_test(NAME test_ipv4_networks
  SOURCES test_ipv4_networks.cpp
  DEPS atlas_server
)

atlas_add_test(NAME test_server_app
  SOURCES test_server_app.cpp
  DEPS atlas_server
)

atlas_add_test(NAME test_script_app
  SOURCES test_script_app.cpp
  DEPS atlas_server
)

atlas_add_test(NAME test_entity_app
  SOURCES test_entity_app.cpp
  DEPS atlas_server
)

# ── Machined ────────────────────────────────────────────────────────────────

atlas_add_test(NAME test_process_registry
  SOURCES test_process_registry.cpp
  DEPS atlas_machined_lib
)

atlas_add_test(NAME test_listener_manager
  SOURCES test_listener_manager.cpp
  DEPS atlas_machined_lib
)

atlas_add_test(NAME test_watcher_forwarder
  SOURCES test_watcher_forwarder.cpp
  DEPS atlas_machined_lib
)

# ── CLR Script ──────────────────────────────────────────────────────────────

atlas_add_test(NAME test_clr_marshal
  SOURCES test_clr_marshal.cpp
  DEPS atlas_clrscript
)

atlas_add_test(NAME test_native_api_provider
  SOURCES test_native_api_provider.cpp
  DEPS atlas_clrscript
)

atlas_add_test(NAME test_clr_error
  SOURCES test_clr_error.cpp
  DEPS atlas_clrscript
)

atlas_add_test(NAME test_clr_invoke
  SOURCES test_clr_invoke.cpp
  DEPS atlas_clrscript
)

atlas_add_test(NAME test_clr_object
  SOURCES test_clr_object.cpp
  DEPS atlas_clrscript
)

# CLR tests that load atlas_engine shared library at runtime.
# Paths are supplied via env vars set in test properties.

add_executable(test_native_api_exports
  test_paths.h
  test_native_api_exports.cpp
)
target_link_libraries(test_native_api_exports PRIVATE GTest::gtest_main atlas_platform)
target_compile_definitions(test_native_api_exports PRIVATE _CRT_SECURE_NO_WARNINGS)
add_dependencies(test_native_api_exports atlas_engine)
add_test(NAME test_native_api_exports COMMAND test_native_api_exports)
set_tests_properties(test_native_api_exports PROPERTIES
  LABELS "unit"
  ENVIRONMENT "ATLAS_ENGINE_RLOC=$<TARGET_FILE:atlas_engine>"
)
set_target_properties(test_native_api_exports PROPERTIES FOLDER "Test/Unit")

add_executable(test_clr_host
  test_paths.h
  test_clr_host.cpp
)
target_link_libraries(test_clr_host PRIVATE GTest::gtest_main atlas_clrscript)
target_compile_definitions(test_clr_host PRIVATE _CRT_SECURE_NO_WARNINGS)
add_dependencies(test_clr_host atlas_engine atlas_smoke_test_dll)
add_test(NAME test_clr_host COMMAND test_clr_host)
set_tests_properties(test_clr_host PROPERTIES
  LABELS "unit"
  ENVIRONMENT
    "ATLAS_RUNTIME_CONFIG_RLOC=${CMAKE_BINARY_DIR}/runtime/atlas_server.runtimeconfig.json;ATLAS_SMOKE_TEST_DLL_RLOC=$<GENEX_EVAL:$<TARGET_PROPERTY:atlas_smoke_test_dll,DOTNET_ASSEMBLY>>"
)
set_target_properties(test_clr_host PROPERTIES FOLDER "Test/Unit")

add_executable(test_clr_callback
  test_paths.h
  test_clr_callback.cpp
)
target_link_libraries(test_clr_callback PRIVATE GTest::gtest_main atlas_clrscript atlas_platform)
target_compile_definitions(test_clr_callback PRIVATE _CRT_SECURE_NO_WARNINGS)
add_dependencies(test_clr_callback atlas_engine atlas_runtime_test_dll)
add_test(NAME test_clr_callback COMMAND test_clr_callback)
set_tests_properties(test_clr_callback PROPERTIES
  LABELS "unit"
  ENVIRONMENT
    "ATLAS_ENGINE_RLOC=$<TARGET_FILE:atlas_engine>;ATLAS_RUNTIME_CONFIG_RLOC=${CMAKE_BINARY_DIR}/runtime/atlas_server.runtimeconfig.json;ATLAS_RUNTIME_TEST_DLL_RLOC=$<GENEX_EVAL:$<TARGET_PROPERTY:atlas_runtime_test_dll,DOTNET_ASSEMBLY>>"
)
set_target_properties(test_clr_callback PROPERTIES FOLDER "Test/Unit")

add_executable(test_clr_script_engine
  test_paths.h
  test_clr_script_engine.cpp
)
target_link_libraries(test_clr_script_engine PRIVATE GTest::gtest_main atlas_clrscript atlas_platform)
target_compile_definitions(test_clr_script_engine PRIVATE _CRT_SECURE_NO_WARNINGS)
add_dependencies(test_clr_script_engine atlas_engine atlas_runtime_test_dll)
add_test(NAME test_clr_script_engine COMMAND test_clr_script_engine)
set_tests_properties(test_clr_script_engine PROPERTIES
  LABELS "unit"
  ENVIRONMENT
    "ATLAS_ENGINE_RLOC=$<TARGET_FILE:atlas_engine>;ATLAS_RUNTIME_CONFIG_RLOC=${CMAKE_BINARY_DIR}/runtime/atlas_server.runtimeconfig.json;ATLAS_RUNTIME_TEST_DLL_RLOC=$<GENEX_EVAL:$<TARGET_PROPERTY:atlas_runtime_test_dll,DOTNET_ASSEMBLY>>"
)
set_target_properties(test_clr_script_engine PROPERTIES FOLDER "Test/Unit")

# ── Server component tests ──────────────────────────────────────────────────

atlas_add_test(NAME test_dbapp_messages
  SOURCES test_dbapp_messages.cpp
  DEPS atlas_serialization atlas_dbapp_lib
)

atlas_add_test(NAME test_checkout_manager
  SOURCES test_checkout_manager.cpp
  DEPS atlas_dbapp_lib
)

atlas_add_test(NAME test_entity_id_allocator
  SOURCES test_entity_id_allocator.cpp
  DEPS atlas_dbapp_lib
)

atlas_add_test(NAME test_dbapp_entity_id_messages
  SOURCES test_dbapp_entity_id_messages.cpp
  DEPS atlas_serialization atlas_dbapp_lib
)

atlas_add_test(NAME test_base_entity
  SOURCES test_base_entity.cpp
  DEPS atlas_baseapp_lib
)

atlas_add_test(NAME test_baseapp_cell_routing
  SOURCES test_baseapp_cell_routing.cpp
  DEPS atlas_baseapp_lib atlas_network
)

atlas_add_test(NAME test_entity_manager
  SOURCES test_entity_manager.cpp
  DEPS atlas_baseapp_lib
)

atlas_add_test(NAME test_baseapp_messages
  SOURCES test_baseapp_messages.cpp
  DEPS atlas_serialization atlas_baseapp_lib
)

atlas_add_test(NAME test_id_client
  SOURCES test_id_client.cpp
  DEPS atlas_baseapp_lib
)

atlas_add_test(NAME test_delta_forwarder
  SOURCES test_delta_forwarder.cpp
  DEPS atlas_baseapp_lib
)

atlas_add_test(NAME test_login_messages
  SOURCES test_login_messages.cpp
  DEPS atlas_serialization atlas_loginapp_lib
)

atlas_add_test(NAME test_login_rollback
  SOURCES test_login_rollback.cpp
  DEPS atlas_baseapp_lib atlas_dbapp_lib atlas_loginapp_lib
)

atlas_add_test(NAME test_baseappmgr_messages
  SOURCES test_baseappmgr_messages.cpp
  DEPS atlas_serialization atlas_baseappmgr_lib
)
