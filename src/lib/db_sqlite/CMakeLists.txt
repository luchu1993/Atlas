# atlas_db_sqlite — SQLite database backend (statically linked sqlite3)

atlas_add_library(atlas_db_sqlite
    SOURCES
        sqlite_database.cpp
    DEPS
        atlas_foundation
        atlas_entitydef
        atlas_network
        sqlite3
)
