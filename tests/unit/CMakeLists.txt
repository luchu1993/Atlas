# Atlas unit tests

atlas_add_test(test_hello
    SOURCES test_hello.cpp
)

atlas_add_test(test_platform_config
    SOURCES test_platform_config.cpp
    DEPS atlas_platform_config
)

atlas_add_test(test_error
    SOURCES test_error.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_string_utils
    SOURCES test_string_utils.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_log
    SOURCES test_log.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_time
    SOURCES test_time.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_timer_queue
    SOURCES test_timer_queue.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_intrusive_ptr
    SOURCES test_intrusive_ptr.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_pool_allocator
    SOURCES test_pool_allocator.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_containers
    SOURCES test_containers.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_filesystem
    SOURCES test_filesystem.cpp
    DEPS atlas_platform
)

atlas_add_test(test_threading
    SOURCES test_threading.cpp
    DEPS atlas_platform
)

atlas_add_test(test_binary_stream
    SOURCES test_binary_stream.cpp
    DEPS atlas_serialization
)

atlas_add_test(test_data_section
    SOURCES test_data_section.cpp
    DEPS atlas_serialization
)

atlas_add_test(test_math
    SOURCES test_math.cpp
    DEPS atlas_math
)

atlas_add_test(test_address
    SOURCES test_address.cpp
    DEPS atlas_network
)

atlas_add_test(test_socket
    SOURCES test_socket.cpp
    DEPS atlas_network
)

atlas_add_test(test_event_dispatcher
    SOURCES test_event_dispatcher.cpp
    DEPS atlas_network
)

atlas_add_test(test_message
    SOURCES test_message.cpp
    DEPS atlas_network
)

atlas_add_test(test_bundle
    SOURCES test_bundle.cpp
    DEPS atlas_network
)

atlas_add_test(test_channel
    SOURCES test_channel.cpp
    DEPS atlas_network
)

atlas_add_test(test_network_interface
    SOURCES test_network_interface.cpp
    DEPS atlas_network
)

atlas_add_test(test_bg_task_manager
    SOURCES test_bg_task_manager.cpp
    DEPS atlas_network
)

atlas_add_test(test_seq_num
    SOURCES test_seq_num.cpp
    DEPS atlas_network
)

atlas_add_test(test_reliable_udp
    SOURCES test_reliable_udp.cpp
    DEPS atlas_network
)

atlas_add_test(test_script_events
    SOURCES test_script_events.cpp
    DEPS atlas_script
)

atlas_add_test(test_script_value
    SOURCES test_script_value.cpp
    DEPS atlas_script
)

atlas_add_test(test_machined_types
    SOURCES test_machined_types.cpp
    DEPS atlas_network
)

atlas_add_test(test_signal_handler
    SOURCES test_signal_handler.cpp
    DEPS atlas_platform
)

atlas_add_test(test_log_sinks
    SOURCES test_log_sinks.cpp
    DEPS atlas_foundation
)

atlas_add_test(test_dynamic_library
    SOURCES test_dynamic_library.cpp
    DEPS atlas_platform
)

# ── ScriptPhase 2: Marshal layer ─────────────────────────────────────────────

atlas_add_test(test_clr_marshal
    SOURCES test_clr_marshal.cpp
    DEPS atlas_clrscript
)

# ── ScriptPhase 2: NativeApi provider + atlas_engine shared library ──────────

atlas_add_test(test_native_api_provider
    SOURCES test_native_api_provider.cpp
    DEPS atlas_clrscript
)

atlas_add_test(test_native_api_exports
    SOURCES test_native_api_exports.cpp
    DEPS atlas_platform
)

# Use $<TARGET_FILE_DIR:atlas_engine> so the path is correct for both
# single-config generators (Ninja: bin/) and multi-config (MSVC: bin/Debug/).
target_compile_definitions(test_native_api_exports PRIVATE
    ATLAS_ENGINE_DIR="$<TARGET_FILE_DIR:atlas_engine>"
)

# Ensure atlas_engine is built before the export-check test runs.
add_dependencies(test_native_api_exports atlas_engine)

# ── ScriptPhase 2: ClrError / ClrInvoke / ClrObject ─────────────────────────

atlas_add_test(test_clr_error
    SOURCES test_clr_error.cpp
    DEPS atlas_clrscript
)

atlas_add_test(test_clr_invoke
    SOURCES test_clr_invoke.cpp
    DEPS atlas_clrscript
)

atlas_add_test(test_clr_object
    SOURCES test_clr_object.cpp
    DEPS atlas_clrscript
)

# ── ScriptPhase 1: CLR host smoke test ───────────────────────────────────────

# Build the C# smoke test assembly before running the C++ test
atlas_build_csharp_project(tests/csharp/Atlas.SmokeTest)

atlas_add_test(test_clr_host
    SOURCES test_clr_host.cpp
    DEPS atlas_clrscript
)

target_compile_definitions(test_clr_host PRIVATE
    ATLAS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ATLAS_BINARY_DIR="${CMAKE_BINARY_DIR}"
)

# Ensure the C# assembly is built before the C++ test runs
add_dependencies(test_clr_host csharp_tests_csharp_Atlas_SmokeTest)

# On Windows, copy nethost.dll next to the test binary so gtest_discover_tests
# and ctest can find it without requiring it to be on the system PATH.
if(WIN32 AND DOTNET_NETHOST_DLL)
    add_custom_command(TARGET test_clr_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DOTNET_NETHOST_DLL}"
            "$<TARGET_FILE_DIR:test_clr_host>/nethost.dll"
        COMMENT "Copying nethost.dll alongside test_clr_host"
    )
endif()

# ── ScriptPhase 2: C# → C++ callback integration test ───────────────────────

# Build Atlas.Runtime (src/csharp) — produces Atlas.Runtime.dll
atlas_build_csharp_project(src/csharp/Atlas.Runtime)

# Build Atlas.RuntimeTest — produces Atlas.RuntimeTest.dll + copies Atlas.Runtime.dll
atlas_build_csharp_project(tests/csharp/Atlas.RuntimeTest)

# RuntimeTest project-references Atlas.Runtime, so build Atlas.Runtime first.
add_dependencies(csharp_tests_csharp_Atlas_RuntimeTest csharp_src_csharp_Atlas_Runtime)

atlas_add_test(test_clr_callback
    SOURCES test_clr_callback.cpp
    DEPS atlas_clrscript atlas_platform
)

set(_runtime_test_out_dir
    "${CMAKE_BINARY_DIR}/csharp/tests/csharp/Atlas.RuntimeTest")

target_compile_definitions(test_clr_callback PRIVATE
    ATLAS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ATLAS_BINARY_DIR="${CMAKE_BINARY_DIR}"
    ATLAS_ENGINE_DIR="$<TARGET_FILE_DIR:atlas_engine>"
    ATLAS_RUNTIME_TEST_DIR="${_runtime_test_out_dir}"
)

add_dependencies(test_clr_callback
    csharp_tests_csharp_Atlas_RuntimeTest
    atlas_engine
)

# Copy nethost.dll and atlas_engine.dll next to the test binary / managed assembly.
if(WIN32 AND DOTNET_NETHOST_DLL)
    add_custom_command(TARGET test_clr_callback POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DOTNET_NETHOST_DLL}"
            "$<TARGET_FILE_DIR:test_clr_callback>/nethost.dll"
        COMMENT "Copying nethost.dll alongside test_clr_callback"
    )
endif()

# Copy atlas_engine.dll into the Atlas.RuntimeTest output dir so that both
# DynamicLibrary::load() and C# [LibraryImport("atlas_engine")] resolve to
# the same DLL instance (avoiding duplicate global state from two DLL copies).
add_custom_command(TARGET test_clr_callback POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:atlas_engine>"
        "${_runtime_test_out_dir}/$<TARGET_FILE_NAME:atlas_engine>"
    COMMENT "Copying atlas_engine to Atlas.RuntimeTest output dir"
)

# ── ScriptPhase 3: ClrScriptEngine lifecycle integration test ────────────────

atlas_add_test(test_clr_script_engine
    SOURCES test_clr_script_engine.cpp
    DEPS atlas_clrscript atlas_platform
)

target_compile_definitions(test_clr_script_engine PRIVATE
    ATLAS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    ATLAS_BINARY_DIR="${CMAKE_BINARY_DIR}"
    ATLAS_ENGINE_DIR="$<TARGET_FILE_DIR:atlas_engine>"
    ATLAS_RUNTIME_TEST_DIR="${_runtime_test_out_dir}"
)

add_dependencies(test_clr_script_engine
    csharp_tests_csharp_Atlas_RuntimeTest
    atlas_engine
)

if(WIN32 AND DOTNET_NETHOST_DLL)
    add_custom_command(TARGET test_clr_script_engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DOTNET_NETHOST_DLL}"
            "$<TARGET_FILE_DIR:test_clr_script_engine>/nethost.dll"
        COMMENT "Copying nethost.dll alongside test_clr_script_engine"
    )
endif()
