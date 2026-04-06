// Python.h is included via py_error.hpp -> py_object.hpp (must come first)
#include "pyscript/py_error.hpp"

#include "foundation/log.hpp"

namespace atlas
{

auto check_python_error() -> Result<void>
{
    if (!PyErr_Occurred())
    {
        return Result<void>{};
    }

    auto msg = format_python_error();
    PyErr_Clear();

    return Error(ErrorCode::ScriptError, std::move(msg));
}

auto format_python_error() -> std::string
{
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);

    if (!type)
    {
        return "Unknown Python error";
    }

    PyErr_NormalizeException(&type, &value, &traceback);

    std::string result;

    // Type name
    if (type)
    {
        auto* type_name = PyObject_GetAttrString(type, "__name__");
        if (type_name)
        {
            const char* name = PyUnicode_AsUTF8(type_name);
            if (name)
            {
                result += name;
            }
            Py_DECREF(type_name);
        }
    }

    // Message
    if (value)
    {
        auto* str_obj = PyObject_Str(value);
        if (str_obj)
        {
            const char* msg = PyUnicode_AsUTF8(str_obj);
            if (msg)
            {
                if (!result.empty())
                {
                    result += ": ";
                }
                result += msg;
            }
            Py_DECREF(str_obj);
        }
    }

    // Restore the exception so caller can decide to clear or keep
    PyErr_Restore(type, value, traceback);

    if (result.empty())
    {
        result = "Unknown Python error";
    }

    return result;
}

void clear_python_error()
{
    PyErr_Clear();
}

void set_python_error(const Error& error)
{
    PyObject* exception_type = nullptr;
    switch (error.code())
    {
        case ErrorCode::ScriptTypeError:
            exception_type = PyExc_TypeError;
            break;
        case ErrorCode::ScriptValueError:
            exception_type = PyExc_ValueError;
            break;
        case ErrorCode::ScriptImportError:
            exception_type = PyExc_ImportError;
            break;
        case ErrorCode::ScriptRuntimeError:
        case ErrorCode::ScriptError:
        default:
            exception_type = PyExc_RuntimeError;
            break;
    }

    std::string msg(error.message());
    PyErr_SetString(exception_type, msg.c_str());
}

} // namespace atlas
