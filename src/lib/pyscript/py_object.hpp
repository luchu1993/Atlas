#pragma once

// Python.h must come before standard headers on Windows.
// On MSVC debug builds, Python.h tries to link python3XX_d.lib which is
// typically not available. Temporarily undefine _DEBUG to link release Python.
#if defined(_MSC_VER)
#   pragma warning(push)
#   pragma warning(disable: 4100 4244 4996)
#   if defined(_DEBUG)
#       undef _DEBUG
#       include <Python.h>
#       define _DEBUG
#   else
#       include <Python.h>
#   endif
#else
#   include <Python.h>
#endif
#if defined(_MSC_VER)
#   pragma warning(pop)
#endif

#include "foundation/error.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace atlas
{

// ============================================================================
// PyObjectPtr — RAII wrapper for PyObject*
// ============================================================================
//
// Manages a single owned (strong) reference to a Python object.
//
// Construction:
//   PyObjectPtr(raw)          — steals a new reference (caller gives up ownership)
//   PyObjectPtr::borrow(raw)  — borrows (increments refcount)
//
// Thread safety: NOT thread-safe. Caller must hold the GIL when constructing,
// copying, moving, or destroying instances.

class PyObjectPtr
{
public:
    // Default: null
    PyObjectPtr() = default;

    // Steal a new reference (caller gives up ownership).
    explicit PyObjectPtr(PyObject* obj) : ptr_(obj) {}

    // Borrow a reference (increments refcount).
    [[nodiscard]] static auto borrow(PyObject* obj) -> PyObjectPtr
    {
        if (obj)
        {
            Py_INCREF(obj);
        }
        return PyObjectPtr(obj);
    }

    ~PyObjectPtr()
    {
        if (ptr_)
        {
            Py_DECREF(ptr_);
        }
    }

    // Copy: increment refcount
    PyObjectPtr(const PyObjectPtr& other) : ptr_(other.ptr_)
    {
        if (ptr_)
        {
            Py_XINCREF(ptr_);
        }
    }

    PyObjectPtr& operator=(const PyObjectPtr& other)
    {
        if (this != &other)
        {
            PyObjectPtr tmp(other);
            std::swap(ptr_, tmp.ptr_);
        }
        return *this;
    }

    // Move: transfer ownership
    PyObjectPtr(PyObjectPtr&& other) noexcept : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    PyObjectPtr& operator=(PyObjectPtr&& other) noexcept
    {
        if (this != &other)
        {
            if (ptr_)
            {
                Py_DECREF(ptr_);
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // ---- Access ----

    [[nodiscard]] auto get() const -> PyObject* { return ptr_; }

    // Release ownership (caller takes responsibility for Py_DECREF).
    [[nodiscard]] auto release() -> PyObject*
    {
        auto* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

    [[nodiscard]] explicit operator bool() const { return ptr_ != nullptr; }

    // ---- Attribute access ----

    [[nodiscard]] auto get_attr(std::string_view name) const -> PyObjectPtr
    {
        std::string name_str(name);
        return PyObjectPtr(PyObject_GetAttrString(ptr_, name_str.c_str()));
    }

    auto set_attr(std::string_view name, const PyObjectPtr& value) -> Result<void>
    {
        std::string name_str(name);
        if (PyObject_SetAttrString(ptr_, name_str.c_str(), value.ptr_) < 0)
        {
            PyErr_Clear();
            return Error(ErrorCode::ScriptError,
                "Failed to set attribute: " + name_str);
        }
        return Result<void>{};
    }

    // ---- Callable interface ----

    // Call with no args.
    [[nodiscard]] auto call() const -> PyObjectPtr
    {
        return PyObjectPtr(PyObject_CallNoArgs(ptr_));
    }

    // Call with positional args (must be a tuple).
    [[nodiscard]] auto call(const PyObjectPtr& args) const -> PyObjectPtr
    {
        return PyObjectPtr(PyObject_CallObject(ptr_, args.ptr_));
    }

    // Call with args and kwargs.
    [[nodiscard]] auto call(const PyObjectPtr& args,
                            const PyObjectPtr& kwargs) const -> PyObjectPtr
    {
        return PyObjectPtr(PyObject_Call(ptr_, args.ptr_, kwargs.ptr_));
    }

    // Call a named method with optional args tuple.
    [[nodiscard]] auto call_method(std::string_view name,
                                   const PyObjectPtr& args = {}) const
        -> PyObjectPtr
    {
        auto method = get_attr(name);
        if (!method)
        {
            return {};
        }
        if (args)
        {
            return method.call(args);
        }
        return method.call();
    }

    // ---- Type checks ----

    [[nodiscard]] auto is_none() const -> bool { return ptr_ == Py_None; }
    [[nodiscard]] auto is_int() const -> bool { return ptr_ && PyLong_Check(ptr_); }
    [[nodiscard]] auto is_float() const -> bool { return ptr_ && PyFloat_Check(ptr_); }
    [[nodiscard]] auto is_string() const -> bool { return ptr_ && PyUnicode_Check(ptr_); }
    [[nodiscard]] auto is_bytes() const -> bool { return ptr_ && PyBytes_Check(ptr_); }
    [[nodiscard]] auto is_list() const -> bool { return ptr_ && PyList_Check(ptr_); }
    [[nodiscard]] auto is_dict() const -> bool { return ptr_ && PyDict_Check(ptr_); }
    [[nodiscard]] auto is_tuple() const -> bool { return ptr_ && PyTuple_Check(ptr_); }
    [[nodiscard]] auto is_callable() const -> bool { return ptr_ && PyCallable_Check(ptr_); }

    // ---- String representation ----

    [[nodiscard]] auto repr() const -> std::string
    {
        if (!ptr_)
        {
            return "<null>";
        }
        auto* r = PyObject_Repr(ptr_);
        if (!r)
        {
            PyErr_Clear();
            return "<repr failed>";
        }
        const char* s = PyUnicode_AsUTF8(r);
        std::string result(s ? s : "");
        Py_DECREF(r);
        return result;
    }

    [[nodiscard]] auto str() const -> std::string
    {
        if (!ptr_)
        {
            return "<null>";
        }
        auto* r = PyObject_Str(ptr_);
        if (!r)
        {
            PyErr_Clear();
            return "<str failed>";
        }
        const char* s = PyUnicode_AsUTF8(r);
        std::string result(s ? s : "");
        Py_DECREF(r);
        return result;
    }

private:
    PyObject* ptr_{nullptr};
};

} // namespace atlas
