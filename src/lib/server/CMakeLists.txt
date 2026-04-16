# atlas_server — Server framework base classes (ServerApp, ScriptApp, EntityApp, BWConfig)

atlas_add_library(atlas_server
    SOURCES
        watcher.cc
        ipv4_networks.cc
        updatable.cc
        server_config.cc
        server_app.cc
        manager_app.cc
        script_app.cc
        entity_app.cc
        machined_client.cc
    DEPS
        atlas_foundation
        atlas_network
        atlas_platform
        atlas_serialization
        atlas_script
        atlas_clrscript
        atlas_entitydef
)
