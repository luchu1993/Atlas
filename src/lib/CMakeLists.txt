# src/lib/CMakeLists.txt — dispatch to all library subdirectories

# Layer 0: no internal deps
add_subdirectory(platform)
add_subdirectory(foundation)

# Layer 1: foundation-level
add_subdirectory(math)
add_subdirectory(script)

# Layer 2: mid-level
add_subdirectory(serialization)
add_subdirectory(entitydef)
add_subdirectory(network)
add_subdirectory(coro)

# Layer 3: server framework + database
add_subdirectory(server)
add_subdirectory(db)
add_subdirectory(db_sqlite)
add_subdirectory(db_xml)
add_subdirectory(db_mysql)
add_subdirectory(clrscript)

# Layer 3: Phase 10 spatial index (used by CellApp)
add_subdirectory(space)

# Placeholders
add_subdirectory(connection)
add_subdirectory(resmgr)
add_subdirectory(physics)
