# loginapp — Client authentication and login handling

atlas_add_library(atlas_loginapp_lib
    SOURCES
        loginapp.cc
    DEPS
        atlas_foundation
        atlas_network
        atlas_server
        atlas_coro
)

# Cross-server headers (baseappmgr messages, etc.)
target_include_directories(atlas_loginapp_lib
    PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/server>
)

atlas_add_executable(atlas_loginapp
    SOURCES
        main.cc
    DEPS
        atlas_loginapp_lib
)
