#include <gtest/gtest.h>
#include "pyscript/py_interpreter.hpp"
#include "pyscript/py_module.hpp"
#include "pyscript/py_type.hpp"

using namespace atlas;

// ============================================================================
// Module-level functions
// ============================================================================

static PyObject* my_add(PyObject*, PyObject* args)
{
    int a = 0, b = 0;
    if (!PyArg_ParseTuple(args, "ii", &a, &b))
    {
        return nullptr;
    }
    return PyLong_FromLong(a + b);
}

static PyObject* my_greet(PyObject*, PyObject* args)
{
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name))
    {
        return nullptr;
    }
    return PyUnicode_FromFormat("Hello, %s!", name);
}

// ============================================================================
// Test fixture
// ============================================================================

class PyModuleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!PyInterpreter::is_initialized())
        {
            (void)PyInterpreter::initialize();
        }
    }
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(PyModuleTest, BuildEmptyModule)
{
    auto result = PyModuleBuilder("test_empty_mod").build();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_TRUE(static_cast<bool>(*result));
}

TEST_F(PyModuleTest, ModuleWithFunction)
{
    auto result = PyModuleBuilder("test_math_mod")
        .add_function("add", my_add, METH_VARARGS, "Add two integers")
        .build();
    ASSERT_TRUE(result.has_value());

    // Import the module from Python
    auto mod = PyInterpreter::import("test_math_mod");
    ASSERT_TRUE(mod.has_value()) << mod.error().message();

    // Call the function
    auto add_func = mod->get_attr("add");
    ASSERT_TRUE(add_func.is_callable());

    PyObjectPtr args(PyTuple_Pack(2, PyLong_FromLong(3), PyLong_FromLong(4)));
    auto result_val = add_func.call(args);
    ASSERT_TRUE(result_val.is_int());
    EXPECT_EQ(PyLong_AsLong(result_val.get()), 7);
}

TEST_F(PyModuleTest, ModuleWithIntConstant)
{
    auto result = PyModuleBuilder("test_const_mod")
        .add_int_constant("VERSION", 42)
        .build();
    ASSERT_TRUE(result.has_value());

    auto mod = PyInterpreter::import("test_const_mod");
    ASSERT_TRUE(mod.has_value());

    auto ver = mod->get_attr("VERSION");
    ASSERT_TRUE(ver.is_int());
    EXPECT_EQ(PyLong_AsLong(ver.get()), 42);
}

TEST_F(PyModuleTest, ModuleWithStringConstant)
{
    auto result = PyModuleBuilder("test_str_mod")
        .add_string_constant("ENGINE_NAME", "Atlas")
        .build();
    ASSERT_TRUE(result.has_value());

    auto mod = PyInterpreter::import("test_str_mod");
    ASSERT_TRUE(mod.has_value());

    auto name = mod->get_attr("ENGINE_NAME");
    ASSERT_TRUE(name.is_string());
    EXPECT_STREQ(PyUnicode_AsUTF8(name.get()), "Atlas");
}

TEST_F(PyModuleTest, ModuleWithType)
{
    // Build a type first
    auto type_result = PyTypeBuilder("ModTestType").build();
    ASSERT_TRUE(type_result.has_value());

    // Build module with the type
    auto mod_result = PyModuleBuilder("test_typed_mod")
        .add_type("ModTestType", *type_result)
        .build();
    ASSERT_TRUE(mod_result.has_value());

    // Import and access the type
    auto mod = PyInterpreter::import("test_typed_mod");
    ASSERT_TRUE(mod.has_value());

    auto type_obj = mod->get_attr("ModTestType");
    ASSERT_TRUE(static_cast<bool>(type_obj));
    EXPECT_TRUE(PyType_Check(type_obj.get()));
}

// ============================================================================
// Review fix: vector reallocation dangling pointer in PyModuleBuilder.
// Same fix as PyTypeBuilder — c_str() pointers deferred to build().
// ============================================================================

static PyObject* my_multiply(PyObject*, PyObject* args)
{
    int a = 0, b = 0;
    if (!PyArg_ParseTuple(args, "ii", &a, &b)) return nullptr;
    return PyLong_FromLong(a * b);
}

static PyObject* my_negate(PyObject*, PyObject* args)
{
    int a = 0;
    if (!PyArg_ParseTuple(args, "i", &a)) return nullptr;
    return PyLong_FromLong(-a);
}

TEST_F(PyModuleTest, ManyFunctionsNoPointerCorruption)
{
    auto result = PyModuleBuilder("test_stress_mod")
        .add_function("add", my_add, METH_VARARGS)
        .add_function("greet", my_greet, METH_VARARGS)
        .add_function("multiply", my_multiply, METH_VARARGS)
        .add_function("negate", my_negate, METH_VARARGS)
        .add_int_constant("CONST1", 100)
        .add_string_constant("CONST2", "test")
        .build();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto mod = PyInterpreter::import("test_stress_mod");
    ASSERT_TRUE(mod.has_value());

    // Verify all functions are callable with correct names
    PyObjectPtr args_add(PyTuple_Pack(2, PyLong_FromLong(3), PyLong_FromLong(4)));
    auto add_result = mod->get_attr("add").call(args_add);
    ASSERT_TRUE(add_result.is_int());
    EXPECT_EQ(PyLong_AsLong(add_result.get()), 7);

    PyObjectPtr args_mul(PyTuple_Pack(2, PyLong_FromLong(5), PyLong_FromLong(6)));
    auto mul_result = mod->get_attr("multiply").call(args_mul);
    ASSERT_TRUE(mul_result.is_int());
    EXPECT_EQ(PyLong_AsLong(mul_result.get()), 30);

    PyObjectPtr args_neg(PyTuple_Pack(1, PyLong_FromLong(42)));
    auto neg_result = mod->get_attr("negate").call(args_neg);
    ASSERT_TRUE(neg_result.is_int());
    EXPECT_EQ(PyLong_AsLong(neg_result.get()), -42);
}

TEST_F(PyModuleTest, CallFunctionFromExec)
{
    // Build module
    auto result = PyModuleBuilder("test_exec_mod")
        .add_function("greet", my_greet, METH_VARARGS)
        .build();
    ASSERT_TRUE(result.has_value());

    // Call from Python exec
    auto exec_result = PyInterpreter::exec(
        "import test_exec_mod\n"
        "result = test_exec_mod.greet('World')\n");
    EXPECT_TRUE(exec_result.has_value()) << exec_result.error().message();

    // Verify result
    auto main_mod = PyInterpreter::import("__main__");
    ASSERT_TRUE(main_mod.has_value());
    auto py_result = main_mod->get_attr("result");
    ASSERT_TRUE(py_result.is_string());
    EXPECT_STREQ(PyUnicode_AsUTF8(py_result.get()), "Hello, World!");
}
