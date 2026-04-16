# atlas_db_xml — XML file database backend (development/testing)

atlas_add_library(atlas_db_xml
    SOURCES
        xml_database.cc
    DEPS
        atlas_foundation
        atlas_serialization
        atlas_entitydef
        atlas_network
)
