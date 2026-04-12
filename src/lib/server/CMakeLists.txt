# atlas_server — Server framework base classes (ServerApp, ScriptApp, EntityApp, BWConfig)

atlas_add_library(atlas_server
    SOURCES
        watcher.cpp
        ipv4_networks.cpp
        updatable.cpp
        server_config.cpp
        server_app.cpp
        manager_app.cpp
        script_app.cpp
        entity_app.cpp
        machined_client.cpp
    DEPS
        atlas_foundation
        atlas_network
        atlas_platform
        atlas_serialization
        atlas_script
        atlas_clrscript
        atlas_entitydef
)
