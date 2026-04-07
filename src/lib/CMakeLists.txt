# Atlas core libraries
# Order matters: dependencies must come before dependents.

add_subdirectory(platform)
add_subdirectory(foundation)
add_subdirectory(math)
add_subdirectory(serialization)
add_subdirectory(resmgr)
add_subdirectory(network)
add_subdirectory(script)
# add_subdirectory(clrscript)  # ScriptPhase 1 时启用
add_subdirectory(entitydef)
add_subdirectory(connection)
add_subdirectory(db)
add_subdirectory(db_mysql)
add_subdirectory(db_xml)
add_subdirectory(chunk)
add_subdirectory(physics)
add_subdirectory(server)
