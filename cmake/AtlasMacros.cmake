# AtlasMacros.cmake
# Utility macros for building Atlas libraries, executables, and tests.

# ── atlas_add_library ────────────────────────────────────────────────────────
# Usage:
#   atlas_add_library(atlas_network
#       SOURCES src1.cpp src2.cpp
#       DEPS    atlas_platform atlas_foundation
#       PUBLIC_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
#   )
function(atlas_add_library target)
    cmake_parse_arguments(ARG
        ""                          # options (flags)
        ""                          # one-value args
        "SOURCES;DEPS;PUBLIC_INCLUDE_DIRS"  # multi-value args
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "atlas_add_library(${target}): no SOURCES given")
    endif()

    add_library(${target} STATIC ${ARG_SOURCES})

    target_include_directories(${target}
        PUBLIC
            ${ARG_PUBLIC_INCLUDE_DIRS}
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    )

    if(ARG_DEPS)
        target_link_libraries(${target} PUBLIC ${ARG_DEPS})
    endif()

    # All Atlas libraries share the top-level src/lib as an include root
    target_include_directories(${target}
        PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/lib>
    )

    set_target_properties(${target} PROPERTIES
        FOLDER "Libraries"
    )
endfunction()

# ── atlas_add_executable ─────────────────────────────────────────────────────
# Usage:
#   atlas_add_executable(loginapp
#       SOURCES main.cpp login_app.cpp
#       DEPS    atlas_server atlas_network
#   )
function(atlas_add_executable target)
    cmake_parse_arguments(ARG
        ""
        ""
        "SOURCES;DEPS"
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "atlas_add_executable(${target}): no SOURCES given")
    endif()

    add_executable(${target} ${ARG_SOURCES})

    if(ARG_DEPS)
        target_link_libraries(${target} PRIVATE ${ARG_DEPS})
    endif()

    set_target_properties(${target} PROPERTIES
        FOLDER "Applications"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

# ── atlas_add_test ───────────────────────────────────────────────────────────
# Usage:
#   atlas_add_test(test_network
#       SOURCES test_socket.cpp test_channel.cpp
#       DEPS    atlas_network
#   )
function(atlas_add_test target)
    if(NOT ATLAS_BUILD_TESTS)
        return()
    endif()

    cmake_parse_arguments(ARG
        ""
        ""
        "SOURCES;DEPS"
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "atlas_add_test(${target}): no SOURCES given")
    endif()

    add_executable(${target} ${ARG_SOURCES})

    target_link_libraries(${target}
        PRIVATE
            GTest::gtest_main
            ${ARG_DEPS}
    )

    set_target_properties(${target} PROPERTIES
        FOLDER "Tests"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
    )

    include(GoogleTest)
    gtest_discover_tests(${target}
        DISCOVERY_TIMEOUT 30
    )
endfunction()
