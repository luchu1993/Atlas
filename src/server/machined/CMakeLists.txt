# machined — Machine daemon for process registration and service discovery

atlas_add_library(atlas_machined_lib
    SOURCES
        process_registry.cpp
        listener_manager.cpp
        watcher_forwarder.cpp
        machined_app.cpp
    DEPS
        atlas_server
    PUBLIC_INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/src/server
)

atlas_add_executable(machined
    SOURCES
        main.cpp
    DEPS
        atlas_machined_lib
)
