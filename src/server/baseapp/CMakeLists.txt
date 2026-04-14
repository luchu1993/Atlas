# baseapp — entity-bearing server process

atlas_add_library(atlas_baseapp_lib
    SOURCES
        base_entity.cpp
        delta_forwarder.cpp
        entity_manager.cpp
        id_client.cpp
        baseapp_native_provider.cpp
        baseapp.cpp
    DEPS
        atlas_foundation
        atlas_network
        atlas_db
        atlas_entitydef
        atlas_server
        atlas_clrscript
)

# Cross-server message headers
target_include_directories(atlas_baseapp_lib
    PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/server>
)

atlas_add_executable(atlas_baseapp
    SOURCES
        main.cpp
    DEPS
        atlas_baseapp_lib
)
