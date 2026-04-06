#pragma once

#include "pyscript/py_object.hpp"
#include "foundation/error.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

// ============================================================================
// PyWrapperObject — Python wrapper that holds a pointer to a C++ instance
// ============================================================================
//
// All types created via PyTypeBuilder use this as their instance layout.
// The C++ object can be:
//   - Owned by the wrapper (destructor non-null -> deletes on Python GC)
//   - Borrowed (destructor null -> C++ manages lifetime)

struct PyWrapperObject
{
    PyObject_HEAD
    void* cpp_instance;
    void (*destructor)(void*);
};

// ============================================================================
// py_unwrap — get the C++ instance from a Python wrapper object
// ============================================================================

template <typename T>
[[nodiscard]] auto py_unwrap(PyObject* obj) -> T*
{
    if (!obj) return nullptr;
    auto* wrapper = reinterpret_cast<PyWrapperObject*>(obj);
    return static_cast<T*>(wrapper->cpp_instance);
}

// ============================================================================
// py_wrap — wrap a C++ instance into a Python object of the given type
// ============================================================================
//
// If own=true, the wrapper will delete the C++ object when Python GC
// collects it. For proper typed destruction, use py_wrap_owned<T>.

[[nodiscard]] auto py_wrap(PyTypeObject* type, void* instance, bool own = false)
    -> PyObjectPtr;

// ============================================================================
// py_wrap_owned — wrap with type-safe destructor
// ============================================================================

template <typename T>
[[nodiscard]] auto py_wrap_owned(PyTypeObject* type, T* instance) -> PyObjectPtr
{
    // Create wrapper that will call delete on the T* when collected
    auto result = py_wrap(type, instance, false);
    if (result)
    {
        auto* wrapper = reinterpret_cast<PyWrapperObject*>(result.get());
        wrapper->destructor = [](void* p) { delete static_cast<T*>(p); };
    }
    return result;
}

// ============================================================================
// PyTypeBuilder — builder for creating Python types from C++ classes
// ============================================================================
//
// Uses PyType_Spec + PyType_Slot (modern Python 3 heap types).
//
// Usage:
//   auto result = PyTypeBuilder("mymodule.MyType")
//       .set_doc("My custom type")
//       .add_method("do_stuff", my_do_stuff_func)
//       .add_readonly("name", my_name_getter)
//       .build();

class PyTypeBuilder
{
public:
    explicit PyTypeBuilder(std::string_view name);
    ~PyTypeBuilder();

    PyTypeBuilder(const PyTypeBuilder&) = delete;
    PyTypeBuilder& operator=(const PyTypeBuilder&) = delete;

    // Set type documentation
    auto set_doc(std::string_view doc) -> PyTypeBuilder&;

    // Set base type (default: object)
    auto set_base(PyTypeObject* base) -> PyTypeBuilder&;

    // Add a method. The function must have signature:
    //   PyObject*(PyObject* self, PyObject* args)
    // Use py_unwrap<T>(self) inside to get the C++ instance.
    auto add_method(std::string_view name, PyCFunction func,
                    int flags = METH_VARARGS,
                    std::string_view doc = "") -> PyTypeBuilder&;

    // Add a property with getter and optional setter.
    // getter: PyObject*(PyObject* self, void* closure)
    // setter: int(PyObject* self, PyObject* value, void* closure) -- null for readonly
    auto add_property(std::string_view name,
                      getter get_func, setter set_func = nullptr,
                      std::string_view doc = "") -> PyTypeBuilder&;

    // Add a read-only property
    auto add_readonly(std::string_view name, getter get_func,
                      std::string_view doc = "") -> PyTypeBuilder&;

    // Build and return the new type. The type is a heap type (Py_TPFLAGS_HEAPTYPE).
    // Returns the type object or an error.
    [[nodiscard]] auto build() -> Result<PyTypeObject*>;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atlas
