# atlas_db_mysql — MySQL database backend (optional, requires libmysqlclient)

option(ATLAS_DB_MYSQL "Build MySQL database backend" OFF)

if(ATLAS_DB_MYSQL)
    find_package(MySQL REQUIRED)

    atlas_add_library(atlas_db_mysql
        SOURCES
            mysql_database.cpp
            mysql_connection_pool.cpp
            mysql_connection.cpp
        DEPS
            atlas_foundation
            atlas_serialization
            atlas_entitydef
            atlas_network
            atlas_db
    )

    target_link_libraries(atlas_db_mysql PUBLIC ${MySQL_LIBRARIES})
    target_include_directories(atlas_db_mysql PUBLIC ${MySQL_INCLUDE_DIRS})
    target_compile_definitions(atlas_db_mysql PUBLIC ATLAS_DB_MYSQL)
else()
    # Provide an empty interface target so other targets can safely link to it
    add_library(atlas_db_mysql INTERFACE)
endif()
