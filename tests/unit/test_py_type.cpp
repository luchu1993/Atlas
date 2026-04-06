#include <gtest/gtest.h>
#include "pyscript/py_interpreter.hpp"
#include "pyscript/py_type.hpp"
#include "pyscript/py_convert.hpp"

using namespace atlas;

// ============================================================================
// A simple C++ class to expose to Python
// ============================================================================

struct Counter
{
    int value = 0;
    void increment() { ++value; }
    void add(int n) { value += n; }
    int get() const { return value; }
    void reset() { value = 0; }
};

// ============================================================================
// PyCFunction trampolines for Counter
// ============================================================================

static PyObject* counter_increment(PyObject* self, PyObject*)
{
    auto* c = py_unwrap<Counter>(self);
    if (!c)
    {
        PyErr_SetString(PyExc_RuntimeError, "null instance");
        return nullptr;
    }
    c->increment();
    Py_RETURN_NONE;
}

static PyObject* counter_add(PyObject* self, PyObject* args)
{
    auto* c = py_unwrap<Counter>(self);
    if (!c)
    {
        PyErr_SetString(PyExc_RuntimeError, "null instance");
        return nullptr;
    }
    int n = 0;
    if (!PyArg_ParseTuple(args, "i", &n))
    {
        return nullptr;
    }
    c->add(n);
    Py_RETURN_NONE;
}

static PyObject* counter_get_value(PyObject* self, void*)
{
    auto* c = py_unwrap<Counter>(self);
    if (!c)
    {
        PyErr_SetString(PyExc_RuntimeError, "null instance");
        return nullptr;
    }
    return PyLong_FromLong(c->get());
}

static int counter_set_value(PyObject* self, PyObject* val, void*)
{
    auto* c = py_unwrap<Counter>(self);
    if (!c)
    {
        PyErr_SetString(PyExc_RuntimeError, "null instance");
        return -1;
    }
    if (!PyLong_Check(val))
    {
        PyErr_SetString(PyExc_TypeError, "Expected int");
        return -1;
    }
    c->value = static_cast<int>(PyLong_AsLong(val));
    return 0;
}

// ============================================================================
// Test fixture
// ============================================================================

class PyTypeTest : public ::testing::Test
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

TEST_F(PyTypeTest, BuildEmptyType)
{
    auto result = PyTypeBuilder("TestEmpty").build();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_NE(*result, nullptr);
}

TEST_F(PyTypeTest, BuildTypeWithMethod)
{
    auto result = PyTypeBuilder("CounterType1")
        .add_method("increment", counter_increment, METH_NOARGS, "Increment counter")
        .build();
    ASSERT_TRUE(result.has_value()) << result.error().message();
}

TEST_F(PyTypeTest, WrapAndUnwrap)
{
    auto type_result = PyTypeBuilder("CounterType2").build();
    ASSERT_TRUE(type_result.has_value());

    Counter c;
    c.value = 42;
    auto obj = py_wrap(*type_result, &c, false);
    ASSERT_TRUE(static_cast<bool>(obj));

    auto* unwrapped = py_unwrap<Counter>(obj.get());
    ASSERT_NE(unwrapped, nullptr);
    EXPECT_EQ(unwrapped->value, 42);
    EXPECT_EQ(unwrapped, &c);
}

TEST_F(PyTypeTest, WrapOwnedDeletesOnCollection)
{
    auto type_result = PyTypeBuilder("CounterType3").build();
    ASSERT_TRUE(type_result.has_value());

    auto* c = new Counter();
    c->value = 99;

    {
        auto obj = py_wrap_owned(*type_result, c);
        ASSERT_TRUE(static_cast<bool>(obj));
        EXPECT_EQ(py_unwrap<Counter>(obj.get())->value, 99);
        // obj goes out of scope — destructor should delete c
    }
    // c is now deleted. Can't access it safely.
    // Just verify no crash.
}

TEST_F(PyTypeTest, CallMethodFromPython)
{
    auto type_result = PyTypeBuilder("CounterType4")
        .add_method("increment", counter_increment, METH_NOARGS)
        .add_method("add", counter_add, METH_VARARGS)
        .build();
    ASSERT_TRUE(type_result.has_value()) << type_result.error().message();

    Counter c;
    c.value = 10;
    auto obj = py_wrap(*type_result, &c, false);
    ASSERT_TRUE(static_cast<bool>(obj));

    // Call increment()
    auto inc_method = obj.get_attr("increment");
    ASSERT_TRUE(inc_method.is_callable());
    auto result1 = inc_method.call();
    EXPECT_EQ(c.value, 11);

    // Call add(5)
    auto add_method = obj.get_attr("add");
    ASSERT_TRUE(add_method.is_callable());
    PyObjectPtr args5(PyTuple_Pack(1, PyLong_FromLong(5)));
    auto result2 = add_method.call(args5);
    EXPECT_EQ(c.value, 16);
}

TEST_F(PyTypeTest, PropertyGetSet)
{
    auto type_result = PyTypeBuilder("CounterType5")
        .add_property("value", counter_get_value, counter_set_value, "Counter value")
        .build();
    ASSERT_TRUE(type_result.has_value());

    Counter c;
    c.value = 7;
    auto obj = py_wrap(*type_result, &c, false);
    ASSERT_TRUE(static_cast<bool>(obj));

    // Get property
    auto val = obj.get_attr("value");
    ASSERT_TRUE(val.is_int());
    EXPECT_EQ(PyLong_AsLong(val.get()), 7);

    // Set property
    PyObjectPtr new_val(PyLong_FromLong(42));
    auto set_result = obj.set_attr("value", new_val);
    EXPECT_TRUE(set_result.has_value());
    EXPECT_EQ(c.value, 42);
}

TEST_F(PyTypeTest, ReadonlyPropertyRejectsSet)
{
    auto type_result = PyTypeBuilder("CounterType6")
        .add_readonly("value", counter_get_value, "Read-only value")
        .build();
    ASSERT_TRUE(type_result.has_value());

    Counter c;
    c.value = 5;
    auto obj = py_wrap(*type_result, &c, false);

    // Get works
    auto val = obj.get_attr("value");
    EXPECT_EQ(PyLong_AsLong(val.get()), 5);

    // Set should fail (read-only)
    PyObjectPtr new_val(PyLong_FromLong(99));
    auto set_result = obj.set_attr("value", new_val);
    EXPECT_FALSE(set_result.has_value());
}

TEST_F(PyTypeTest, TypeWithDocstring)
{
    auto type_result = PyTypeBuilder("DocType")
        .set_doc("A type with documentation")
        .build();
    ASSERT_TRUE(type_result.has_value());
    // Type object exists and is valid
    EXPECT_NE(*type_result, nullptr);
}

// ============================================================================
// Review fix: vector reallocation dangling pointer.
// Adding many methods triggers vector reallocation in method_names/methods.
// The fix defers c_str() pointer assignment to build() after all insertions.
// ============================================================================

// Additional trampolines for the stress test
static PyObject* counter_reset(PyObject* self, PyObject*)
{
    auto* c = py_unwrap<Counter>(self);
    if (!c) { PyErr_SetString(PyExc_RuntimeError, "null"); return nullptr; }
    c->reset();
    Py_RETURN_NONE;
}

static PyObject* counter_get(PyObject* self, PyObject*)
{
    auto* c = py_unwrap<Counter>(self);
    if (!c) { PyErr_SetString(PyExc_RuntimeError, "null"); return nullptr; }
    return PyLong_FromLong(c->get());
}

TEST_F(PyTypeTest, ManyMethodsNoPointerCorruption)
{
    // Adding 5+ methods forces multiple vector reallocations.
    // Before the fix, method name pointers would dangle after reallocation.
    auto type_result = PyTypeBuilder("StressType")
        .add_method("increment", counter_increment, METH_NOARGS)
        .add_method("add", counter_add, METH_VARARGS)
        .add_method("reset", counter_reset, METH_NOARGS)
        .add_method("get", counter_get, METH_NOARGS)
        .add_property("value", counter_get_value, counter_set_value)
        .add_readonly("ro_value", counter_get_value)
        .build();
    ASSERT_TRUE(type_result.has_value()) << type_result.error().message();

    Counter c;
    c.value = 0;
    auto obj = py_wrap(*type_result, &c, false);
    ASSERT_TRUE(static_cast<bool>(obj));

    // Call each method to verify name pointers are valid
    (void)obj.get_attr("increment").call();
    EXPECT_EQ(c.value, 1);

    PyObjectPtr args3(PyTuple_Pack(1, PyLong_FromLong(9)));
    (void)obj.get_attr("add").call(args3);
    EXPECT_EQ(c.value, 10);

    auto get_result = obj.get_attr("get").call();
    ASSERT_TRUE(get_result.is_int());
    EXPECT_EQ(PyLong_AsLong(get_result.get()), 10);

    (void)obj.get_attr("reset").call();
    EXPECT_EQ(c.value, 0);

    // Properties should also work
    auto val = obj.get_attr("value");
    EXPECT_EQ(PyLong_AsLong(val.get()), 0);

    auto ro_val = obj.get_attr("ro_value");
    EXPECT_EQ(PyLong_AsLong(ro_val.get()), 0);
}
