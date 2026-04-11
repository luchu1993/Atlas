# Atlas integration tests
#
# These tests exercise multi-component interactions:
#   - TCP round-trip between two NetworkInterface instances
#   - MachinedApp full registration / heartbeat / query flow
#   - RUDP bidirectional messaging between two NetworkInterface instances

atlas_add_test(test_tcp_echo_roundtrip
    SOURCES test_tcp_echo_roundtrip.cpp
    DEPS atlas_network
)

atlas_add_test(test_machined_registration
    SOURCES test_machined_registration.cpp
    DEPS atlas_machined_lib atlas_server atlas_network
)

atlas_add_test(test_rudp_two_network_interfaces
    SOURCES test_rudp_two_network_interfaces.cpp
    DEPS atlas_network
)
