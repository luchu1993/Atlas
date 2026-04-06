# atlas_math — Vector, Matrix, Quaternion math library

atlas_add_library(atlas_math
    SOURCES
        matrix4.cpp
        quaternion.cpp
    DEPS
        atlas_platform_config
)
