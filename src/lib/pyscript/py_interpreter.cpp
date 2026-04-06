// Python.h is included via py_interpreter.hpp -> py_object.hpp (must come first)
#include "pyscript/py_interpreter.hpp"

#include "foundation/log.hpp"
#include "pyscript/py_error.hpp"

#include <cassert>
#include <format>

namespace atlas
{

std::atomic<bool> PyInterpreter::initialized_{false};

auto PyInterpreter::initialize() -> Result<void>
{
    return initialize(Config{});
}

auto PyInterpreter::initialize(const Config& config) -> Result<void>
{
    if (initialized_.load(std::memory_order_acquire))
    {
        return Error(ErrorCode::AlreadyExists, "Python interpreter is already initialized");
    }

    PyConfig config_py;
    if (config.isolated)
    {
        PyConfig_InitIsolatedConfig(&config_py);
    }
    else
    {
        PyConfig_InitPythonConfig(&config_py);
    }

    config_py.install_signal_handlers = config.install_signal_handlers ? 1 : 0;

    // Set program name
    auto* wname = Py_DecodeLocale(config.program_name.c_str(), nullptr);
    if (wname)
    {
        auto status = PyConfig_SetString(&config_py, &config_py.program_name, wname);
        PyMem_RawFree(wname);
        if (PyStatus_Exception(status))
        {
            PyConfig_Clear(&config_py);
            return Error(ErrorCode::ScriptError, "Failed to set Python program name");
        }
    }

    // Set Python home if specified
    if (!config.python_home.empty())
    {
        auto whome = config.python_home.wstring();
        auto status = PyConfig_SetString(&config_py, &config_py.home, whome.c_str());
        if (PyStatus_Exception(status))
        {
            PyConfig_Clear(&config_py);
            return Error(ErrorCode::ScriptError, "Failed to set Python home");
        }
    }

    // Initialize the interpreter
    auto status = Py_InitializeFromConfig(&config_py);
    PyConfig_Clear(&config_py);

    if (PyStatus_Exception(status))
    {
        return Error(ErrorCode::ScriptError,
                     std::format("Failed to initialize Python: {}",
                                 status.err_msg ? status.err_msg : "unknown"));
    }

    // Add custom paths to sys.path
    for (const auto& path : config.paths)
    {
        auto result = add_sys_path(path);
        if (!result)
        {
            ATLAS_LOG_WARNING("Failed to add sys.path entry: {}", path.string());
        }
    }

    initialized_.store(true, std::memory_order_release);
    ATLAS_LOG_INFO("Python {} initialized", version());
    return Result<void>{};
}

void PyInterpreter::finalize()
{
    if (!initialized_.load(std::memory_order_acquire))
    {
        return;
    }

    // Run garbage collection before shutdown
    PyGC_Collect();

    if (Py_FinalizeEx() < 0)
    {
        ATLAS_LOG_WARNING("Python finalization reported errors");
    }

    initialized_.store(false, std::memory_order_release);
    ATLAS_LOG_INFO("Python interpreter finalized");
}

auto PyInterpreter::is_initialized() -> bool
{
    return initialized_.load(std::memory_order_acquire);
}

auto PyInterpreter::exec(std::string_view code) -> Result<void>
{
    assert(PyGILState_Check() && "GIL must be held when calling exec()");

    std::string code_str(code);
    int result = PyRun_SimpleString(code_str.c_str());
    if (result != 0)
    {
        // Check if Python set an exception with details
        if (PyErr_Occurred())
        {
            auto msg = format_python_error();
            PyErr_Clear();
            return Error(ErrorCode::ScriptError, std::move(msg));
        }
        return Error(ErrorCode::ScriptError, "Python exec failed (no exception details available)");
    }
    return Result<void>{};
}

auto PyInterpreter::import(std::string_view module_name) -> Result<PyObjectPtr>
{
    assert(PyGILState_Check() && "GIL must be held when calling import()");

    std::string name(module_name);
    PyObject* mod = PyImport_ImportModule(name.c_str());
    if (!mod)
    {
        if (PyErr_Occurred())
        {
            auto msg = format_python_error();
            PyErr_Clear();
            return Error(ErrorCode::ScriptImportError, std::move(msg));
        }
        return Error(ErrorCode::ScriptImportError,
                     std::format("Failed to import module '{}'", name));
    }
    return PyObjectPtr(mod);
}

auto PyInterpreter::add_sys_path(const std::filesystem::path& path) -> Result<void>
{
    // PySys_GetObject returns a borrowed reference
    PyObject* sys_path = PySys_GetObject("path");
    if (!sys_path)
    {
        return Error(ErrorCode::ScriptError, "Failed to get sys.path");
    }

    auto wpath = path.wstring();
    PyObject* py_path =
        PyUnicode_FromWideChar(wpath.c_str(), static_cast<Py_ssize_t>(wpath.size()));
    if (!py_path)
    {
        return Error(ErrorCode::ScriptError, "Failed to create Python string for path");
    }

    if (PyList_Append(sys_path, py_path) < 0)
    {
        Py_DECREF(py_path);
        return Error(ErrorCode::ScriptError, "Failed to append to sys.path");
    }

    Py_DECREF(py_path);
    return Result<void>{};
}

auto PyInterpreter::version() -> std::string_view
{
    return Py_GetVersion();
}

}  // namespace atlas
