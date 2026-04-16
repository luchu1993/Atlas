# baseappmgr — BaseApp cluster manager, load balancing

atlas_add_library(atlas_baseappmgr_lib
    SOURCES
        baseappmgr.cc
    DEPS
        atlas_foundation
        atlas_network
        atlas_server
)

# Cross-server headers live under src/server/<process>/ — expose the root
target_include_directories(atlas_baseappmgr_lib
    PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/server>
)

atlas_add_executable(atlas_baseappmgr
    SOURCES
        main.cc
    DEPS
        atlas_baseappmgr_lib
)
