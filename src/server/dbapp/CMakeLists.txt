# dbapp — Database proxy, async entity persistence

atlas_add_library(atlas_dbapp_lib
    SOURCES
        checkout_manager.cc
        dbapp.cc
        entity_id_allocator.cc
    DEPS
        atlas_foundation
        atlas_network
        atlas_db
        atlas_db_xml
        atlas_entitydef
        atlas_server
)

target_include_directories(atlas_dbapp_lib
    PUBLIC $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/server>
)

atlas_add_executable(atlas_dbapp
    SOURCES
        main.cc
    DEPS
        atlas_dbapp_lib
)
