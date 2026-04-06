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
