#pragma once

#include "foundation/error.hpp"
#include "pyscript/py_object.hpp"

namespace atlas
{

// Register the 'atlas' built-in module into Python.
// Must be called after PyInterpreter::initialize().
[[nodiscard]] auto register_atlas_module() -> Result<PyObjectPtr>;

}  // namespace atlas
