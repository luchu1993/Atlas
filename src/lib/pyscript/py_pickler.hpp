#pragma once

#include "pyscript/py_object.hpp"
#include "foundation/error.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace atlas
{

// ============================================================================
// PyPickler — Pickle serialization for Python objects
// ============================================================================
//
// Uses Python 3 `pickle` module (not cPickle -- removed in Python 3).
// Thread safety: requires GIL.

class PyPickler
{
public:
    // Initialize: imports pickle module. Call after PyInterpreter::initialize().
    [[nodiscard]] static auto initialize() -> Result<void>;

    // Serialize a Python object to bytes (pickle.dumps with protocol 5).
    [[nodiscard]] static auto pickle(PyObject* obj) -> Result<std::vector<std::byte>>;

    // Deserialize bytes to a Python object (pickle.loads).
    [[nodiscard]] static auto unpickle(std::span<const std::byte> data) -> Result<PyObjectPtr>;

private:
    static PyObjectPtr dumps_;
    static PyObjectPtr loads_;
};

} // namespace atlas
