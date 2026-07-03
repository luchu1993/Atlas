# atlas_db_mysql — MySQL database backend (MariaDB Connector/C)

if(ATLAS_DB_MYSQL)
  add_library(atlas_db_mysql STATIC
    mysql_database.cc
  )
  target_include_directories(atlas_db_mysql
    PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
    PRIVATE "${ATLAS_MARIADB_INCLUDE_DIR}")
  target_link_libraries(atlas_db_mysql
    PUBLIC
      atlas_db_iface
      atlas_entitydef
      atlas_foundation
      atlas_network
      mariadbclient
      atlas_compiler_options
  )

  # Plugin DLL — loaded at runtime by the database factory via DynamicLibrary.
  add_library(atlas_db_mysql_plugin SHARED mysql_database_plugin.cc)
  target_compile_definitions(atlas_db_mysql_plugin PRIVATE ATLAS_DB_BACKEND_EXPORTS)
  target_link_libraries(atlas_db_mysql_plugin PRIVATE atlas_db_mysql)
  set_target_properties(atlas_db_mysql_plugin PROPERTIES
    OUTPUT_NAME "atlas_db_mysql"
    FOLDER "Plugins"
    # The import lib (.lib on Windows) must not collide with the static library
    # of the same name; redirect it to a build-tree subdirectory that nothing
    # references at runtime.
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/plugins"
  )
  if(UNIX)
    set_target_properties(atlas_db_mysql_plugin PROPERTIES CXX_VISIBILITY_PRESET hidden)
  endif()
endif()
