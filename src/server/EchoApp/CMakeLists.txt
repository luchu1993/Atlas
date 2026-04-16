# atlas_echoapp — minimal framework verification process

atlas_add_executable(atlas_echoapp
    SOURCES
        echo_app.cc
        main.cc
    DEPS
        atlas_server
)

# Place include root so echo_app.hpp can be found when including from main.cc
target_include_directories(atlas_echoapp
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
