# atlas_entitydef — Entity definition system (DataType, EntityDescription, Registry)

atlas_add_library(atlas_entitydef
    SOURCES
        entity_def_registry.cc
    DEPS
        atlas_foundation
        atlas_serialization
)
