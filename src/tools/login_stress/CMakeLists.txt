# login_stress — End-to-end LoginApp/BaseApp stress client

add_executable(login_stress main.cc)

target_link_libraries(login_stress PRIVATE
    atlas_network
    atlas_server
    atlas_baseapp_lib
    atlas_loginapp_lib
)

set_target_properties(login_stress PROPERTIES
    FOLDER "Applications"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
