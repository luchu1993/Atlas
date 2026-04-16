# atlas_db — Database abstraction interface (IDatabase) + factory

atlas_add_library(atlas_db
    SOURCES
        database_factory.cc
    DEPS
        atlas_foundation
        atlas_network
        atlas_entitydef
        atlas_db_sqlite
        atlas_db_xml
        atlas_platform
)
