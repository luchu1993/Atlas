# atlas_db_sqlite — SQLite database backend (runtime-loaded sqlite3 library)

atlas_add_library(atlas_db_sqlite
    SOURCES
        sqlite_database.cpp
    DEPS
        atlas_foundation
        atlas_entitydef
        atlas_network
        atlas_platform
)
