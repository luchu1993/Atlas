#include <gtest/gtest.h>
#include "pyscript/py_interpreter.hpp"
#include "pyscript/py_object.hpp"

using namespace atlas;

class PyObjectTest : public ::testing::Test
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

TEST_F(PyObjectTest, DefaultIsNull)
{
    PyObjectPtr obj;
    EXPECT_FALSE(static_cast<bool>(obj));
    EXPECT_EQ(obj.get(), nullptr);
}

TEST_F(PyObjectTest, StealReference)
{
    // PyLong_FromLong returns a new reference
    PyObjectPtr obj(PyLong_FromLong(42));
    EXPECT_TRUE(static_cast<bool>(obj));
    EXPECT_TRUE(obj.is_int());
}

TEST_F(PyObjectTest, BorrowReference)
{
    // Use a list (not a small int) — small ints are immortalized in Python 3.12+
    // and their refcount is a special value that doesn't change.
    PyObject* raw = PyList_New(0);
    auto initial_refcnt = Py_REFCNT(raw);

    {
        auto borrowed = PyObjectPtr::borrow(raw);
        EXPECT_GT(Py_REFCNT(raw), initial_refcnt);
    }
    // After borrowed goes out of scope, refcount should be restored
    EXPECT_EQ(Py_REFCNT(raw), initial_refcnt);
    Py_DECREF(raw);
}

TEST_F(PyObjectTest, CopyIncrementsRefcount)
{
    // Use a list to avoid immortal object refcount issues
    PyObjectPtr a(PyList_New(0));
    auto refcnt_before = Py_REFCNT(a.get());

    PyObjectPtr b = a;  // copy
    EXPECT_GT(Py_REFCNT(a.get()), refcnt_before);
    EXPECT_EQ(a.get(), b.get());
}

TEST_F(PyObjectTest, MoveTransfersOwnership)
{
    PyObjectPtr a(PyLong_FromLong(8));
    auto* raw = a.get();

    PyObjectPtr b = std::move(a);
    EXPECT_EQ(b.get(), raw);
    EXPECT_EQ(a.get(), nullptr);
}

TEST_F(PyObjectTest, ReleaseReturnsAndNulls)
{
    PyObjectPtr obj(PyLong_FromLong(99));
    auto* raw = obj.release();
    EXPECT_NE(raw, nullptr);
    EXPECT_EQ(obj.get(), nullptr);
    Py_DECREF(raw);  // manual cleanup
}

TEST_F(PyObjectTest, TypeChecks)
{
    EXPECT_TRUE(PyObjectPtr(PyLong_FromLong(1)).is_int());
    EXPECT_TRUE(PyObjectPtr(PyFloat_FromDouble(1.0)).is_float());
    EXPECT_TRUE(PyObjectPtr(PyUnicode_FromString("hello")).is_string());
    EXPECT_TRUE(PyObjectPtr(PyBytes_FromString("data")).is_bytes());
    EXPECT_TRUE(PyObjectPtr(PyList_New(0)).is_list());
    EXPECT_TRUE(PyObjectPtr(PyDict_New()).is_dict());
    EXPECT_TRUE(PyObjectPtr(PyTuple_New(0)).is_tuple());
    EXPECT_TRUE(PyObjectPtr::borrow(Py_None).is_none());
}

TEST_F(PyObjectTest, GetSetAttribute)
{
    // Create a simple object via exec
    (void)PyInterpreter::exec("class Obj: pass\ntest_obj = Obj()\ntest_obj.x = 42");
    auto main = PyInterpreter::import("__main__");
    ASSERT_TRUE(main.has_value());

    auto obj = main->get_attr("test_obj");
    ASSERT_TRUE(static_cast<bool>(obj));

    auto x = obj.get_attr("x");
    ASSERT_TRUE(x.is_int());
    EXPECT_EQ(PyLong_AsLong(x.get()), 42);
}

TEST_F(PyObjectTest, CallCallable)
{
    auto builtins = PyInterpreter::import("builtins");
    ASSERT_TRUE(builtins.has_value());

    auto len_func = builtins->get_attr("len");
    ASSERT_TRUE(len_func.is_callable());

    // Call len([1,2,3])
    PyObjectPtr list(PyList_New(3));
    PyList_SetItem(list.get(), 0, PyLong_FromLong(1));
    PyList_SetItem(list.get(), 1, PyLong_FromLong(2));
    PyList_SetItem(list.get(), 2, PyLong_FromLong(3));

    PyObjectPtr args(PyTuple_New(1));
    PyTuple_SetItem(args.get(), 0, list.release());  // steals ref

    auto result = len_func.call(args);
    ASSERT_TRUE(result.is_int());
    EXPECT_EQ(PyLong_AsLong(result.get()), 3);
}

TEST_F(PyObjectTest, ReprAndStr)
{
    PyObjectPtr obj(PyLong_FromLong(42));
    EXPECT_EQ(obj.repr(), "42");
    EXPECT_EQ(obj.str(), "42");
}
