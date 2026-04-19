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
