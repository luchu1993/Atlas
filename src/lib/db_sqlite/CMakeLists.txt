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

# Plugin DLL — loaded at runtime by the database factory via DynamicLibrary.
add_library(atlas_db_sqlite_plugin SHARED sqlite_database_plugin.cc)
target_compile_definitions(atlas_db_sqlite_plugin PRIVATE ATLAS_DB_BACKEND_EXPORTS)
target_link_libraries(atlas_db_sqlite_plugin PRIVATE atlas_db_sqlite)
set_target_properties(atlas_db_sqlite_plugin PROPERTIES
  OUTPUT_NAME "atlas_db_sqlite"
  FOLDER "Plugins"
  # The import lib (.lib on Windows) must not collide with the static library
  # of the same name; redirect it to a build-tree subdirectory that nothing
  # references at runtime.
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/plugins"
)
if(UNIX)
  set_target_properties(atlas_db_sqlite_plugin PROPERTIES CXX_VISIBILITY_PRESET hidden)
endif()
