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
