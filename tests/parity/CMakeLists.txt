atlas_add_test(NAME test_backend_parity_quick
  SOURCES
    test_backend_parity.cpp
    parity_runner.cc
    parity_scenarios.cc
  DEPS atlas_physics atlas_movement_sim
)

if(ATLAS_ENABLE_JOLT)
  target_link_libraries(test_backend_parity_quick PRIVATE atlas_physics_jolt)
  target_compile_definitions(test_backend_parity_quick PRIVATE ATLAS_PARITY_HAS_JOLT)
endif()
