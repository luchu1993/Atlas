# atlas_db_sqlite — SQLite database backend

add_library(atlas_db_sqlite STATIC
  sqlite_database.cc
)

target_include_directories(atlas_db_sqlite PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_db_sqlite
  PUBLIC
    atlas_db_iface
    atlas_entitydef
    atlas_foundation
    atlas_network
    sqlite3
    atlas_compiler_options
)
