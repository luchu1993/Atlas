add_subdirectory(platform)
add_subdirectory(foundation)

add_subdirectory(math)
add_subdirectory(physics)
if(ATLAS_ENABLE_JOLT)
  add_subdirectory(physics_jolt)
endif()
add_subdirectory(navigation)
add_subdirectory(movement_sim)
add_subdirectory(script)

add_subdirectory(serialization)
add_subdirectory(entitydef)
add_subdirectory(network)
add_subdirectory(net_client)
add_subdirectory(coro)

add_subdirectory(server)
add_subdirectory(db)
add_subdirectory(db_sqlite)
add_subdirectory(db_xml)
add_subdirectory(db_mysql)
add_subdirectory(clrscript)

add_subdirectory(space)

add_subdirectory(connection)
add_subdirectory(resmgr)
