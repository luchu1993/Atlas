add_library(atlas_serialization_binary STATIC
  binary_stream.cc
)

target_include_directories(atlas_serialization_binary
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
)

target_link_libraries(atlas_serialization_binary
  PUBLIC atlas_foundation atlas_platform atlas_compiler_options
)

add_library(atlas_serialization STATIC
  data_section.cc
  json_parser.cc
  xml_parser.cc
)

target_include_directories(atlas_serialization
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
)

target_link_libraries(atlas_serialization
  PUBLIC atlas_serialization_binary pugixml rapidjson
)
