# Atlas core libraries
# Order matters: dependencies must come before dependents.

add_subdirectory(platform)
add_subdirectory(foundation)
add_subdirectory(math)
add_subdirectory(serialization)
add_subdirectory(resmgr)
add_subdirectory(network)
add_subdirectory(script)
add_subdirectory(clrscript)
add_subdirectory(entitydef)
add_subdirectory(connection)
add_subdirectory(db)
add_subdirectory(db_mysql)
add_subdirectory(db_xml)
add_subdirectory(chunk)
add_subdirectory(physics)
add_subdirectory(server)

# ── atlas_engine shared library ───────────────────────────────────────────────
#
# Bundles the atlas_* C-linkage export functions into a shared library so that
# C# [LibraryImport("atlas_engine")] P/Invoke can resolve them at runtime.
#
# clr_native_api.cpp is compiled exclusively here (not in any static lib).
# It delegates to the INativeApiProvider registered by the hosting process.
# The static library atlas_clrscript (linked below) provides
# native_api_provider.cpp (set/get_native_api_provider), ClrHost, etc.

add_library(atlas_engine SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/clrscript/clr_native_api.cpp
)

target_compile_definitions(atlas_engine PRIVATE ATLAS_ENGINE_EXPORTS)

# atlas_clrscript carries the PUBLIC include root (src/lib) transitively,
# so atlas_engine can include any header under src/lib without extra setup.
target_link_libraries(atlas_engine
    PRIVATE
        atlas_clrscript
        atlas_foundation
        atlas_platform
)

# On non-Windows: hide all symbols by default; only ATLAS_EXPORT ones are
# visible, preventing global namespace pollution from the static deps.
if(NOT WIN32)
    target_compile_options(atlas_engine PRIVATE -fvisibility=hidden)
endif()

set_target_properties(atlas_engine PROPERTIES
    FOLDER "Libraries"
    # Place the .dll (Windows) and .so (Linux) under bin/ so that both the
    # server executables and the test that loads it via DynamicLibrary can
    # find it in one consistent location.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
