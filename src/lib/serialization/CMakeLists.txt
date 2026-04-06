# atlas_serialization — Binary streams, XML/JSON parsing, DataSection

atlas_add_library(atlas_serialization
    SOURCES
        binary_stream.cpp
        data_section.cpp
        xml_parser.cpp
        json_parser.cpp
    DEPS
        atlas_foundation
        atlas_platform
        pugixml::pugixml
)

# rapidjson is header-only, fetched via FetchContent_Populate
target_include_directories(atlas_serialization
    SYSTEM PRIVATE ${rapidjson_SOURCE_DIR}/include
)
