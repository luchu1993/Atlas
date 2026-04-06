# atlas_pyscript — Python 3 embedding (interpreter, GIL, type conversion, error handling)

atlas_add_library(atlas_pyscript
    SOURCES
        py_interpreter.cpp
        py_error.cpp
    DEPS
        atlas_foundation
        atlas_platform
        Python3::Python
)
