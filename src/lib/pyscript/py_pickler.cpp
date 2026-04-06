#include "pyscript/py_pickler.hpp"
#include "pyscript/py_error.hpp"
#include "foundation/log.hpp"

namespace atlas
{

std::atomic<bool> PyPickler::initialized_{false};
PyObjectPtr PyPickler::dumps_;
PyObjectPtr PyPickler::loads_;

auto PyPickler::initialize() -> Result<void>
{
    if (initialized_.load(std::memory_order_acquire))
    {
        return Result<void>{};
    }

    auto mod = PyObjectPtr(PyImport_ImportModule("pickle"));
    if (!mod)
    {
        clear_python_error();
        return Error(ErrorCode::ScriptImportError, "Failed to import pickle module");
    }

    dumps_ = mod.get_attr("dumps");
    loads_ = mod.get_attr("loads");

    if (!dumps_ || !loads_)
    {
        return Error(ErrorCode::ScriptError, "Failed to get pickle.dumps/loads");
    }

    initialized_.store(true, std::memory_order_release);
    return Result<void>{};
}

void PyPickler::finalize()
{
    dumps_ = {};
    loads_ = {};
    initialized_.store(false, std::memory_order_release);
}

auto PyPickler::pickle(PyObject* obj) -> Result<std::vector<std::byte>>
{
    if (!dumps_)
    {
        return Error(ErrorCode::ScriptError, "PyPickler not initialized");
    }

    // Call pickle.dumps(obj, protocol=5)
    // Protocol 5 supports out-of-band data (Python 3.8+)
    auto py_protocol = PyObjectPtr(PyLong_FromLong(5));
    auto args = PyObjectPtr(PyTuple_Pack(2, obj, py_protocol.get()));
    auto result = dumps_.call(args);

    if (!result)
    {
        auto err = format_python_error();
        clear_python_error();
        return Error(ErrorCode::ScriptError, "pickle.dumps failed: " + err);
    }

    // Result is bytes in Python 3
    if (!PyBytes_Check(result.get()))
    {
        return Error(ErrorCode::ScriptTypeError, "pickle.dumps did not return bytes");
    }

    auto* data = reinterpret_cast<const std::byte*>(PyBytes_AsString(result.get()));
    auto size = static_cast<std::size_t>(PyBytes_Size(result.get()));
    return std::vector<std::byte>(data, data + size);
}

auto PyPickler::unpickle(std::span<const std::byte> data) -> Result<PyObjectPtr>
{
    if (!loads_)
    {
        return Error(ErrorCode::ScriptError, "PyPickler not initialized");
    }

    auto py_bytes = PyObjectPtr(PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(data.data()),
        static_cast<Py_ssize_t>(data.size())));
    if (!py_bytes)
    {
        return Error(ErrorCode::ScriptError, "Failed to create bytes for unpickle");
    }

    auto args = PyObjectPtr(PyTuple_Pack(1, py_bytes.get()));
    auto result = loads_.call(args);

    if (!result)
    {
        auto err = format_python_error();
        clear_python_error();
        return Error(ErrorCode::ScriptError, "pickle.loads failed: " + err);
    }

    return result;
}

} // namespace atlas
