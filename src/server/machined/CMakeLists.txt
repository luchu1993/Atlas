# machined — Machine daemon for process registration and service discovery

atlas_add_library(atlas_machined_lib
    SOURCES
        process_registry.cc
        listener_manager.cc
        watcher_forwarder.cc
        machined_app.cc
    DEPS
        atlas_server
    PUBLIC_INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/src/server
)

atlas_add_executable(machined
    SOURCES
        main.cc
    DEPS
        atlas_machined_lib
)
