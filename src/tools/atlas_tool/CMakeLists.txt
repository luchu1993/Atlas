# atlas_tool — Command-line tool for interacting with machined

add_executable(atlas_tool main.cc)

target_link_libraries(atlas_tool PRIVATE
    atlas_server
)

set_target_properties(atlas_tool PROPERTIES
    FOLDER "Applications"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
