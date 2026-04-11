# dbapp — Database proxy, async entity persistence

atlas_add_library(atlas_dbapp_lib
    SOURCES
        checkout_manager.cpp
        dbapp.cpp
    DEPS
        atlas_foundation
        atlas_network
        atlas_db
        atlas_db_xml
        atlas_entitydef
        atlas_server
)

target_include_directories(atlas_dbapp_lib
    PRIVATE ${CMAKE_SOURCE_DIR}/src/server
)

atlas_add_executable(atlas_dbapp
    SOURCES
        main.cpp
    DEPS
        atlas_dbapp_lib
)
