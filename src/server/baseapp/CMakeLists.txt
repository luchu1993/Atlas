# baseapp — entity-bearing server process

atlas_add_library(atlas_baseapp_lib
    SOURCES
        base_entity.cc
        delta_forwarder.cc
        entity_manager.cc
        id_client.cc
        baseapp_native_provider.cc
        baseapp.cc
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
        main.cc
    DEPS
        atlas_baseapp_lib
)
