# atlas_serialization — split into binary (no third-party deps) and full
# (XML / JSON via pugixml / rapidjson). atlas_network links the binary
# subset only so atlas_net_client.dll can ship without dragging the
# parser dependencies into the Unity client.

# ---- atlas_serialization_binary --------------------------------------------
# Standalone binary stream reader / writer. Zero third-party dependencies;
# safe to link from atlas_network and from any DLL bound for IL2CPP.

add_library(atlas_serialization_binary STATIC
  binary_stream.cc
)

target_include_directories(atlas_serialization_binary
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
)

target_link_libraries(atlas_serialization_binary
  PUBLIC atlas_foundation atlas_platform atlas_compiler_options
)

# ---- atlas_serialization (full) --------------------------------------------
# DataSection + XML + JSON parsers on top of atlas_serialization_binary.
# Server-side configuration loading and entitydef stay on this target;
# the parser transitive deps (pugixml / rapidjson) ride along.

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
