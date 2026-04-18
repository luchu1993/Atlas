# atlas_serialization — binary streams, XML/JSON parsing

add_library(atlas_serialization STATIC
  binary_stream.cc
  data_section.cc
  json_parser.cc
  xml_parser.cc
)

target_include_directories(atlas_serialization PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_serialization
  PUBLIC atlas_foundation atlas_platform pugixml rapidjson atlas_compiler_options
)
