// Python.h is included via py_interpreter.hpp -> py_object.hpp (must come first)
#include "pyscript/py_interpreter.hpp"

#include "pyscript/py_error.hpp"
#include "foundation/log.hpp"

#include <format>

namespace atlas
{

bool PyInterpreter::initialized_ = false;

auto PyInterpreter::initialize(const Config& config) -> Result<void>
{
    if (initialized_)
    {
        return Error(ErrorCode::AlreadyExists,
            "Python interpreter is already initialized");
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

    config_py.install_signal_handlers =
        config.install_signal_handlers ? 1 : 0;

    // Set program name
    auto* wname = Py_DecodeLocale(config.program_name.c_str(), nullptr);
    if (wname)
    {
        PyConfig_SetString(&config_py, &config_py.program_name, wname);
        PyMem_RawFree(wname);
    }

    // Set Python home if specified
    if (!config.python_home.empty())
    {
        auto whome = config.python_home.wstring();
        PyConfig_SetString(&config_py, &config_py.home, whome.c_str());
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
            ATLAS_LOG_WARNING("Failed to add sys.path entry: {}",
                path.string());
        }
    }

    initialized_ = true;
    ATLAS_LOG_INFO("Python {} initialized", version());
    return Result<void>{};
}

void PyInterpreter::finalize()
{
    if (!initialized_)
    {
        return;
    }

    // Run garbage collection before shutdown
    PyGC_Collect();

    if (Py_FinalizeEx() < 0)
    {
        ATLAS_LOG_WARNING("Python finalization reported errors");
    }

    initialized_ = false;
    ATLAS_LOG_INFO("Python interpreter finalized");
}

auto PyInterpreter::is_initialized() -> bool
{
    return initialized_;
}

auto PyInterpreter::exec(std::string_view code) -> Result<void>
{
    std::string code_str(code);
    int result = PyRun_SimpleString(code_str.c_str());
    if (result != 0)
    {
        auto err = check_python_error();
        if (!err)
        {
            return err;
        }
        return Error(ErrorCode::ScriptError,
            "Python exec failed (no exception details available)");
    }
    return Result<void>{};
}

auto PyInterpreter::import(std::string_view module_name) -> Result<PyObjectPtr>
{
    std::string name(module_name);
    PyObject* mod = PyImport_ImportModule(name.c_str());
    if (!mod)
    {
        auto err = check_python_error();
        if (!err)
        {
            return err.error();
        }
        return Error(ErrorCode::ScriptImportError,
            std::format("Failed to import module '{}'", name));
    }
    return PyObjectPtr(mod);  // steals the new reference
}

auto PyInterpreter::add_sys_path(const std::filesystem::path& path)
    -> Result<void>
{
    // PySys_GetObject returns a borrowed reference
    PyObject* sys_path = PySys_GetObject("path");
    if (!sys_path)
    {
        return Error(ErrorCode::ScriptError,
            "Failed to get sys.path");
    }

    auto wpath = path.wstring();
    PyObject* py_path = PyUnicode_FromWideChar(wpath.c_str(),
        static_cast<Py_ssize_t>(wpath.size()));
    if (!py_path)
    {
        return Error(ErrorCode::ScriptError,
            "Failed to create Python string for path");
    }

    if (PyList_Append(sys_path, py_path) < 0)
    {
        Py_DECREF(py_path);
        return Error(ErrorCode::ScriptError,
            "Failed to append to sys.path");
    }

    Py_DECREF(py_path);
    return Result<void>{};
}

auto PyInterpreter::version() -> std::string_view
{
    return Py_GetVersion();
}

} // namespace atlas
