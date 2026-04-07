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
