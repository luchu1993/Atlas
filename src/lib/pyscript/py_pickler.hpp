#pragma once

#include "pyscript/py_object.hpp"
#include "foundation/error.hpp"

#include <atomic>
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
    // Thread-safe (uses internal synchronization).
    [[nodiscard]] static auto initialize() -> Result<void>;

    // Serialize a Python object to bytes (pickle.dumps with protocol 5).
    // Requires GIL.
    [[nodiscard]] static auto pickle(PyObject* obj) -> Result<std::vector<std::byte>>;

    // Deserialize bytes to a Python object (pickle.loads).
    // Requires GIL.
    [[nodiscard]] static auto unpickle(std::span<const std::byte> data) -> Result<PyObjectPtr>;

    // Release cached references. Call from PyInterpreter::finalize().
    static void finalize();

private:
    static std::atomic<bool> initialized_;
    static PyObjectPtr dumps_;
    static PyObjectPtr loads_;
};

} // namespace atlas
