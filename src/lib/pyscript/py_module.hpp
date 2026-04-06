#pragma once

#include "pyscript/py_object.hpp"
#include "foundation/error.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

// ============================================================================
// PyModuleBuilder — builder for creating Python extension modules
// ============================================================================
//
// Creates a Python module and registers it in sys.modules so it can be
// imported by Python scripts.
//
// Usage:
//   auto result = PyModuleBuilder("atlas_math")
//       .add_function("add", my_add_func)
//       .add_type("Vector3", vec3_type)
//       .add_int_constant("VERSION", 1)
//       .build();

class PyModuleBuilder
{
public:
    explicit PyModuleBuilder(std::string_view name);
    ~PyModuleBuilder();

    PyModuleBuilder(const PyModuleBuilder&) = delete;
    PyModuleBuilder& operator=(const PyModuleBuilder&) = delete;

    // Add a function to the module
    auto add_function(std::string_view name, PyCFunction func,
                      int flags = METH_VARARGS,
                      std::string_view doc = "") -> PyModuleBuilder&;

    // Add a type to the module
    auto add_type(std::string_view name, PyTypeObject* type) -> PyModuleBuilder&;

    // Add integer constant
    auto add_int_constant(std::string_view name, long value) -> PyModuleBuilder&;

    // Add string constant
    auto add_string_constant(std::string_view name,
                             std::string_view value) -> PyModuleBuilder&;

    // Build and return the module object
    [[nodiscard]] auto build() -> Result<PyObjectPtr>;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atlas
