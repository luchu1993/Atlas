# src/server/CMakeLists.txt — dispatch to all server components

add_subdirectory(machined)
add_subdirectory(loginapp)
add_subdirectory(baseappmgr)
add_subdirectory(dbapp)
add_subdirectory(baseapp)

# Placeholders
add_subdirectory(cellapp)
add_subdirectory(cellappmgr)
add_subdirectory(dbappmgr)
add_subdirectory(reviver)
