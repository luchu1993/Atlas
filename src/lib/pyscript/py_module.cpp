#include "pyscript/py_module.hpp"

#include "foundation/log.hpp"

namespace atlas
{

// ============================================================================
// Static registry — Impl and PyModuleDef must outlive the module objects,
// because PyMethodDef arrays and module name are referenced by the module.
// ============================================================================

// Registry is a function-local static inside build() to access private Impl

// ============================================================================
// PyModuleBuilder::Impl
// ============================================================================

struct PyModuleBuilder::Impl
{
    std::string name;
    std::vector<PyMethodDef> methods;
    std::vector<std::string> method_names;
    std::vector<std::string> method_docs;

    struct TypeEntry
    {
        std::string name;
        PyTypeObject* type;
    };
    std::vector<TypeEntry> types;

    struct IntConstant
    {
        std::string name;
        long value;
    };
    std::vector<IntConstant> int_constants;

    struct StringConstant
    {
        std::string name;
        std::string value;
    };
    std::vector<StringConstant> string_constants;

    // The PyModuleDef must also persist. Stored here for convenience.
    PyModuleDef module_def{};
};

// ============================================================================
// PyModuleBuilder
// ============================================================================

PyModuleBuilder::PyModuleBuilder(std::string_view name) : impl_(std::make_unique<Impl>())
{
    impl_->name = name;
}

PyModuleBuilder::~PyModuleBuilder() = default;

auto PyModuleBuilder::add_function(std::string_view name, PyCFunction func, int flags,
                                   std::string_view doc) -> PyModuleBuilder&
{
    impl_->method_names.emplace_back(name);
    impl_->method_docs.emplace_back(doc);

    PyMethodDef def{};
    def.ml_name = nullptr;  // fixed up in build()
    def.ml_meth = func;
    def.ml_flags = flags;
    def.ml_doc = nullptr;  // fixed up in build()
    impl_->methods.push_back(def);

    return *this;
}

auto PyModuleBuilder::add_type(std::string_view name, PyTypeObject* type) -> PyModuleBuilder&
{
    impl_->types.push_back({std::string(name), type});
    return *this;
}

auto PyModuleBuilder::add_int_constant(std::string_view name, long value) -> PyModuleBuilder&
{
    impl_->int_constants.push_back({std::string(name), value});
    return *this;
}

auto PyModuleBuilder::add_string_constant(std::string_view name, std::string_view value)
    -> PyModuleBuilder&
{
    impl_->string_constants.push_back({std::string(name), std::string(value)});
    return *this;
}

auto PyModuleBuilder::build() -> Result<PyObjectPtr>
{
    // Fix up string pointers (vectors may have reallocated during add_function)
    for (std::size_t i = 0; i < impl_->methods.size(); ++i)
    {
        impl_->methods[i].ml_name = impl_->method_names[i].c_str();
        if (!impl_->method_docs[i].empty())
        {
            impl_->methods[i].ml_doc = impl_->method_docs[i].c_str();
        }
    }

    // Add sentinel to methods
    impl_->methods.push_back({nullptr, nullptr, 0, nullptr});

    // Move Impl into static registry so it outlives the module object.
    auto* impl = impl_.get();
    get_registry().push_back(std::move(impl_));

    // Set up PyModuleDef (stored inside Impl, which now lives in the registry)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion-null"
#endif
    impl->module_def = PyModuleDef_HEAD_INIT;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    impl->module_def.m_name = impl->name.c_str();
    impl->module_def.m_doc = nullptr;
    impl->module_def.m_size = -1;
    impl->module_def.m_methods = impl->methods.data();

    auto* mod = PyModule_Create(&impl->module_def);
    if (!mod)
    {
        if (PyErr_Occurred())
            PyErr_Clear();
        return Error(ErrorCode::ScriptError, "Failed to create module: " + impl->name);
    }

    // Add types
    for (auto& entry : impl->types)
    {
        Py_INCREF(entry.type);
        if (PyModule_AddObject(mod, entry.name.c_str(), reinterpret_cast<PyObject*>(entry.type)) <
            0)
        {
            Py_DECREF(entry.type);
            Py_DECREF(mod);
            return Error(ErrorCode::ScriptError,
                         "Failed to add type '" + entry.name + "' to module '" + impl->name + "'");
        }
    }

    // Add int constants
    for (auto& c : impl->int_constants)
    {
        PyModule_AddIntConstant(mod, c.name.c_str(), c.value);
    }

    // Add string constants
    for (auto& c : impl->string_constants)
    {
        PyModule_AddStringConstant(mod, c.name.c_str(), c.value.c_str());
    }

    // Register module in sys.modules so it can be imported
    PyObject* sys_modules = PySys_GetObject("modules");
    if (sys_modules)
    {
        PyDict_SetItemString(sys_modules, impl->name.c_str(), mod);
    }

    return PyObjectPtr(mod);  // steals the new reference from PyModule_Create
}

auto PyModuleBuilder::get_registry() -> std::vector<std::unique_ptr<Impl>>&
{
    static std::vector<std::unique_ptr<Impl>> s_registry;
    return s_registry;
}

void PyModuleBuilder::finalize_all()
{
    get_registry().clear();
}

}  // namespace atlas
