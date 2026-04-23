# atlas_db_xml — XML database backend

add_library(atlas_db_xml STATIC
  xml_database.cc
)

target_include_directories(atlas_db_xml PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_db_xml
  PUBLIC
    atlas_db_iface
    atlas_entitydef
    atlas_foundation
    atlas_network
    atlas_serialization
    atlas_compiler_options
)

# Plugin DLL — loaded at runtime by the database factory via DynamicLibrary.
add_library(atlas_db_xml_plugin SHARED xml_database_plugin.cc)
target_compile_definitions(atlas_db_xml_plugin PRIVATE ATLAS_DB_BACKEND_EXPORTS)
target_link_libraries(atlas_db_xml_plugin PRIVATE atlas_db_xml)
set_target_properties(atlas_db_xml_plugin PROPERTIES
  OUTPUT_NAME "atlas_db_xml"
  FOLDER "Plugins"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/plugins"
)
if(UNIX)
  set_target_properties(atlas_db_xml_plugin PROPERTIES CXX_VISIBILITY_PRESET hidden)
endif()
