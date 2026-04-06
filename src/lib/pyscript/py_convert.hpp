#pragma once

#include "foundation/error.hpp"
#include "pyscript/py_object.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::py_convert
{

// ============================================================================
// C++ -> Python conversions
// ============================================================================

[[nodiscard]] inline auto to_python(bool val) -> PyObjectPtr
{
    return PyObjectPtr(PyBool_FromLong(val ? 1 : 0));
}

[[nodiscard]] inline auto to_python(int32_t val) -> PyObjectPtr
{
    return PyObjectPtr(PyLong_FromLong(val));
}

[[nodiscard]] inline auto to_python(int64_t val) -> PyObjectPtr
{
    return PyObjectPtr(PyLong_FromLongLong(val));
}

[[nodiscard]] inline auto to_python(uint32_t val) -> PyObjectPtr
{
    return PyObjectPtr(PyLong_FromUnsignedLong(val));
}

[[nodiscard]] inline auto to_python(uint64_t val) -> PyObjectPtr
{
    return PyObjectPtr(PyLong_FromUnsignedLongLong(val));
}

[[nodiscard]] inline auto to_python(double val) -> PyObjectPtr
{
    return PyObjectPtr(PyFloat_FromDouble(val));
}

[[nodiscard]] inline auto to_python(float val) -> PyObjectPtr
{
    return PyObjectPtr(PyFloat_FromDouble(static_cast<double>(val)));
}

[[nodiscard]] inline auto to_python(std::string_view val) -> PyObjectPtr
{
    return PyObjectPtr(
        PyUnicode_FromStringAndSize(val.data(), static_cast<Py_ssize_t>(val.size())));
}

[[nodiscard]] inline auto to_python(const std::vector<std::byte>& val) -> PyObjectPtr
{
    return PyObjectPtr(PyBytes_FromStringAndSize(reinterpret_cast<const char*>(val.data()),
                                                 static_cast<Py_ssize_t>(val.size())));
}

// ============================================================================
// Python -> C++ conversions
// ============================================================================

template <typename T>
[[nodiscard]] auto from_python(PyObject* obj) -> Result<T>;

template <>
[[nodiscard]] inline auto from_python<bool>(PyObject* obj) -> Result<bool>
{
    if (!obj)
    {
        return Error(ErrorCode::ScriptTypeError, "null object");
    }
    return PyObject_IsTrue(obj) != 0;
}

template <>
[[nodiscard]] inline auto from_python<int32_t>(PyObject* obj) -> Result<int32_t>
{
    if (!obj || !PyLong_Check(obj))
    {
        return Error(ErrorCode::ScriptTypeError, "Expected int");
    }
    long val = PyLong_AsLong(obj);
    if (val == -1 && PyErr_Occurred())
    {
        PyErr_Clear();
        return Error(ErrorCode::ScriptValueError, "Integer overflow");
    }
    return static_cast<int32_t>(val);
}

template <>
[[nodiscard]] inline auto from_python<int64_t>(PyObject* obj) -> Result<int64_t>
{
    if (!obj || !PyLong_Check(obj))
    {
        return Error(ErrorCode::ScriptTypeError, "Expected int");
    }
    long long val = PyLong_AsLongLong(obj);
    if (val == -1 && PyErr_Occurred())
    {
        PyErr_Clear();
        return Error(ErrorCode::ScriptValueError, "Integer overflow");
    }
    return static_cast<int64_t>(val);
}

template <>
[[nodiscard]] inline auto from_python<uint32_t>(PyObject* obj) -> Result<uint32_t>
{
    if (!obj || !PyLong_Check(obj))
    {
        return Error(ErrorCode::ScriptTypeError, "Expected int");
    }
    unsigned long val = PyLong_AsUnsignedLong(obj);
    if (val == static_cast<unsigned long>(-1) && PyErr_Occurred())
    {
        PyErr_Clear();
        return Error(ErrorCode::ScriptValueError, "Integer overflow or negative");
    }
    return static_cast<uint32_t>(val);
}

template <>
[[nodiscard]] inline auto from_python<double>(PyObject* obj) -> Result<double>
{
    if (!obj)
    {
        return Error(ErrorCode::ScriptTypeError, "null object");
    }
    if (PyFloat_Check(obj))
    {
        return PyFloat_AsDouble(obj);
    }
    if (PyLong_Check(obj))
    {
        PyErr_Clear();  // Clear any stale error before calling PyLong_AsDouble
        double val = PyLong_AsDouble(obj);
        if (PyErr_Occurred())
        {
            PyErr_Clear();
            return Error(ErrorCode::ScriptValueError, "Float conversion failed");
        }
        return val;
    }
    return Error(ErrorCode::ScriptTypeError, "Expected float or int");
}

template <>
[[nodiscard]] inline auto from_python<float>(PyObject* obj) -> Result<float>
{
    auto result = from_python<double>(obj);
    if (!result)
    {
        return result.error();
    }
    return static_cast<float>(*result);
}

template <>
[[nodiscard]] inline auto from_python<std::string>(PyObject* obj) -> Result<std::string>
{
    if (!obj)
    {
        return Error(ErrorCode::ScriptTypeError, "null object");
    }
    if (PyUnicode_Check(obj))
    {
        Py_ssize_t size = 0;
        const char* data = PyUnicode_AsUTF8AndSize(obj, &size);
        if (!data)
        {
            PyErr_Clear();
            return Error(ErrorCode::ScriptValueError, "UTF-8 encoding failed");
        }
        return std::string(data, static_cast<std::size_t>(size));
    }
    if (PyBytes_Check(obj))
    {
        return std::string(PyBytes_AsString(obj), static_cast<std::size_t>(PyBytes_Size(obj)));
    }
    return Error(ErrorCode::ScriptTypeError, "Expected str or bytes");
}

template <>
[[nodiscard]] inline auto from_python<std::vector<std::byte>>(PyObject* obj)
    -> Result<std::vector<std::byte>>
{
    if (!obj || !PyBytes_Check(obj))
    {
        return Error(ErrorCode::ScriptTypeError, "Expected bytes");
    }
    auto* data = reinterpret_cast<const std::byte*>(PyBytes_AsString(obj));
    auto size = static_cast<std::size_t>(PyBytes_Size(obj));
    return std::vector<std::byte>(data, data + size);
}

}  // namespace atlas::py_convert
