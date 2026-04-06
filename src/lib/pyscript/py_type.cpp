#include "pyscript/py_type.hpp"

#include "foundation/log.hpp"

#include <cstring>

namespace atlas
{

// ============================================================================
// Static registry — Impl objects must outlive the type objects they create,
// because PyMethodDef and PyGetSetDef arrays are referenced by the type.
// ============================================================================

// Registry is accessed via PyTypeBuilder methods (which can access private Impl)

// ============================================================================
// PyWrapperObject dealloc / new trampolines
// ============================================================================

namespace
{

void wrapper_dealloc(PyObject* self)
{
    auto* wrapper = reinterpret_cast<PyWrapperObject*>(self);
    if (wrapper->destructor && wrapper->cpp_instance)
    {
        wrapper->destructor(wrapper->cpp_instance);
    }
    wrapper->cpp_instance = nullptr;
    wrapper->destructor = nullptr;

    auto* type = Py_TYPE(self);
    type->tp_free(self);
    Py_DECREF(type);  // heap types need this
}

PyObject* wrapper_new(PyTypeObject* type, PyObject*, PyObject*)
{
    auto* self = type->tp_alloc(type, 0);
    if (self)
    {
        auto* wrapper = reinterpret_cast<PyWrapperObject*>(self);
        wrapper->cpp_instance = nullptr;
        wrapper->destructor = nullptr;
    }
    return self;
}

}  // anonymous namespace

// ============================================================================
// py_wrap
// ============================================================================

auto py_wrap(PyTypeObject* type, void* instance, bool own) -> PyObjectPtr
{
    auto* obj = type->tp_alloc(type, 0);
    if (!obj)
        return {};

    auto* wrapper = reinterpret_cast<PyWrapperObject*>(obj);
    wrapper->cpp_instance = instance;
    wrapper->destructor = nullptr;  // caller can set via py_wrap_owned

    if (own)
    {
        // Generic destructor -- only safe for POD-like types.
        // Use py_wrap_owned<T> for proper typed destruction.
    }

    return PyObjectPtr(obj);
}

// ============================================================================
// PyTypeBuilder::Impl
// ============================================================================

struct PyTypeBuilder::Impl
{
    std::string name;
    std::string doc;
    PyTypeObject* base{nullptr};
    std::vector<PyMethodDef> methods;
    std::vector<PyGetSetDef> getsets;
    // Store strings so they don't go out of scope
    std::vector<std::string> method_names;
    std::vector<std::string> method_docs;
    std::vector<std::string> prop_names;
    std::vector<std::string> prop_docs;
};

// ============================================================================
// PyTypeBuilder
// ============================================================================

PyTypeBuilder::PyTypeBuilder(std::string_view name) : impl_(std::make_unique<Impl>())
{
    impl_->name = name;
    // Pre-allocate to reduce reallocation during add_method/add_property
    impl_->method_names.reserve(8);
    impl_->method_docs.reserve(8);
    impl_->methods.reserve(8);
    impl_->prop_names.reserve(8);
    impl_->prop_docs.reserve(8);
    impl_->getsets.reserve(8);
}

PyTypeBuilder::~PyTypeBuilder() = default;

auto PyTypeBuilder::set_doc(std::string_view doc) -> PyTypeBuilder&
{
    impl_->doc = doc;
    return *this;
}

auto PyTypeBuilder::set_base(PyTypeObject* base) -> PyTypeBuilder&
{
    impl_->base = base;
    return *this;
}

auto PyTypeBuilder::add_method(std::string_view name, PyCFunction func, int flags,
                               std::string_view doc) -> PyTypeBuilder&
{
    impl_->method_names.emplace_back(name);
    impl_->method_docs.emplace_back(doc);

    // Store func and flags; name/doc pointers will be fixed up in build()
    // after all strings are finalized (no more vector reallocation).
    PyMethodDef def{};
    def.ml_name = nullptr;  // fixed up in build()
    def.ml_meth = func;
    def.ml_flags = flags;
    def.ml_doc = nullptr;  // fixed up in build()
    impl_->methods.push_back(def);

    return *this;
}

auto PyTypeBuilder::add_property(std::string_view name, getter get_func, setter set_func,
                                 std::string_view doc) -> PyTypeBuilder&
{
    impl_->prop_names.emplace_back(name);
    impl_->prop_docs.emplace_back(doc);

    PyGetSetDef def{};
    def.name = nullptr;  // fixed up in build()
    def.get = get_func;
    def.set = set_func;
    def.doc = nullptr;  // fixed up in build()
    def.closure = nullptr;
    impl_->getsets.push_back(def);

    return *this;
}

auto PyTypeBuilder::add_readonly(std::string_view name, getter get_func, std::string_view doc)
    -> PyTypeBuilder&
{
    return add_property(name, get_func, nullptr, doc);
}

auto PyTypeBuilder::build() -> Result<PyTypeObject*>
{
    // Fix up string pointers — vectors may have reallocated during add_method/add_property.
    // Now that all insertions are done, c_str() pointers are stable.
    for (std::size_t i = 0; i < impl_->methods.size(); ++i)
    {
        impl_->methods[i].ml_name = impl_->method_names[i].c_str();
        if (!impl_->method_docs[i].empty())
        {
            impl_->methods[i].ml_doc = impl_->method_docs[i].c_str();
        }
    }
    for (std::size_t i = 0; i < impl_->getsets.size(); ++i)
    {
        impl_->getsets[i].name = impl_->prop_names[i].c_str();
        if (!impl_->prop_docs[i].empty())
        {
            impl_->getsets[i].doc = impl_->prop_docs[i].c_str();
        }
    }

    // Add sentinel to methods
    impl_->methods.push_back({nullptr, nullptr, 0, nullptr});

    // Add sentinel to getsets
    impl_->getsets.push_back({nullptr, nullptr, nullptr, nullptr, nullptr});

    // Build slot array
    std::vector<PyType_Slot> slots;
    slots.push_back({Py_tp_dealloc, reinterpret_cast<void*>(wrapper_dealloc)});
    slots.push_back({Py_tp_new, reinterpret_cast<void*>(wrapper_new)});
    slots.push_back({Py_tp_methods, impl_->methods.data()});
    slots.push_back({Py_tp_getset, impl_->getsets.data()});

    if (!impl_->doc.empty())
    {
        slots.push_back({Py_tp_doc, const_cast<char*>(impl_->doc.c_str())});
    }

    // Sentinel
    slots.push_back({0, nullptr});

    // Build spec
    PyType_Spec spec{};
    spec.name = impl_->name.c_str();
    spec.basicsize = static_cast<int>(sizeof(PyWrapperObject));
    spec.flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE;
    spec.slots = slots.data();

    PyObject* bases = nullptr;
    if (impl_->base)
    {
        bases = PyTuple_Pack(1, impl_->base);
    }

    auto* type = reinterpret_cast<PyTypeObject*>(PyType_FromSpecWithBases(&spec, bases));
    Py_XDECREF(bases);

    if (!type)
    {
        std::string err_msg = "Failed to create Python type: " + impl_->name;
        if (PyErr_Occurred())
        {
            PyObject* ptype = nullptr;
            PyObject* pvalue = nullptr;
            PyObject* ptb = nullptr;
            PyErr_Fetch(&ptype, &pvalue, &ptb);
            if (pvalue)
            {
                auto* s = PyObject_Str(pvalue);
                if (s)
                {
                    const char* cs = PyUnicode_AsUTF8(s);
                    if (cs)
                    {
                        err_msg += " (";
                        err_msg += cs;
                        err_msg += ")";
                    }
                    Py_DECREF(s);
                }
            }
            Py_XDECREF(ptype);
            Py_XDECREF(pvalue);
            Py_XDECREF(ptb);
        }
        return Error(ErrorCode::ScriptError, std::move(err_msg));
    }

    // Move Impl into static registry so it outlives the type object.
    // PyMethodDef and PyGetSetDef arrays are referenced by the type.
    get_registry().push_back(std::move(impl_));

    return type;
}

auto PyTypeBuilder::get_registry() -> std::vector<std::unique_ptr<Impl>>&
{
    static std::vector<std::unique_ptr<Impl>> s_registry;
    return s_registry;
}

void PyTypeBuilder::finalize_all()
{
    get_registry().clear();
}

}  // namespace atlas
