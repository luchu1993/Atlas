# atlas_client — console client application with embedded CLR

atlas_add_library(atlas_client_lib
    SOURCES
        client_app.cc
        client_native_provider.cc
    DEPS
        atlas_foundation
        atlas_network
        atlas_serialization
        atlas_entitydef
        atlas_clrscript
        atlas_platform
        atlas_server
        atlas_db
)

# Need access to server message headers (login_messages.hpp, baseapp_messages.hpp)
target_include_directories(atlas_client_lib
    PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/server>
)

atlas_add_executable(atlas_client
    SOURCES
        main.cc
    DEPS
        atlas_client_lib
)
