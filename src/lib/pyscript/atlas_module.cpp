#include "pyscript/atlas_module.hpp"

#include "foundation/log.hpp"
#include "foundation/time.hpp"
#include "pyscript/py_convert.hpp"
#include "pyscript/py_module.hpp"

namespace atlas
{

// ============================================================================
// Static C trampoline functions
// ============================================================================

static PyObject* atlas_log_info(PyObject*, PyObject* args)
{
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg))
    {
        return nullptr;
    }
    ATLAS_LOG_INFO("{}", msg);
    Py_RETURN_NONE;
}

static PyObject* atlas_log_warning(PyObject*, PyObject* args)
{
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg))
    {
        return nullptr;
    }
    ATLAS_LOG_WARNING("{}", msg);
    Py_RETURN_NONE;
}

static PyObject* atlas_log_error(PyObject*, PyObject* args)
{
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg))
    {
        return nullptr;
    }
    ATLAS_LOG_ERROR("{}", msg);
    Py_RETURN_NONE;
}

static PyObject* atlas_log_critical(PyObject*, PyObject* args)
{
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s", &msg))
    {
        return nullptr;
    }
    ATLAS_LOG_CRITICAL("{}", msg);
    Py_RETURN_NONE;
}

static PyObject* atlas_server_time(PyObject*, PyObject*)
{
    auto now = Clock::now();
    auto since_epoch = now.time_since_epoch();
    double seconds = std::chrono::duration<double>(since_epoch).count();
    return PyFloat_FromDouble(seconds);
}

// ============================================================================
// Module registration
// ============================================================================

auto register_atlas_module() -> Result<PyObjectPtr>
{
    return PyModuleBuilder("atlas")
        .add_function("log_info", atlas_log_info, METH_VARARGS, "Log info message")
        .add_function("log_warning", atlas_log_warning, METH_VARARGS, "Log warning message")
        .add_function("log_error", atlas_log_error, METH_VARARGS, "Log error message")
        .add_function("log_critical", atlas_log_critical, METH_VARARGS, "Log critical message")
        .add_function("server_time", atlas_server_time, METH_NOARGS, "Get server time in seconds")
        .add_int_constant("VERSION_MAJOR", 0)
        .add_int_constant("VERSION_MINOR", 1)
        .add_string_constant("ENGINE_NAME", "Atlas")
        .build();
}

}  // namespace atlas
