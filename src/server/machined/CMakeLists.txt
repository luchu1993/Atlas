# machined — Machine daemon for process registration and service discovery

add_library(atlas_machined_lib STATIC
    process_registry.cpp
    listener_manager.cpp
    watcher_forwarder.cpp
    machined_app.cpp
)

target_include_directories(atlas_machined_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src/server
)

target_link_libraries(atlas_machined_lib PUBLIC
    atlas_server
)

# machined executable
add_executable(machined main.cpp)

target_link_libraries(machined PRIVATE
    atlas_machined_lib
)

set_target_properties(machined PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
