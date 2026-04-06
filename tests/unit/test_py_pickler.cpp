#include <gtest/gtest.h>
#include "pyscript/py_pickler.hpp"
#include "pyscript/py_interpreter.hpp"
#include "pyscript/py_convert.hpp"

using namespace atlas;

class PyPicklerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!PyInterpreter::is_initialized())
            (void)PyInterpreter::initialize();

        static bool pickler_init = false;
        if (!pickler_init)
        {
            auto result = PyPickler::initialize();
            ASSERT_TRUE(result.has_value()) << result.error().message();
            pickler_init = true;
        }
    }
};

TEST_F(PyPicklerTest, PickleUnpickleInt)
{
    auto py_int = py_convert::to_python(int32_t{42});
    auto data = PyPickler::pickle(py_int.get());
    ASSERT_TRUE(data.has_value()) << data.error().message();
    EXPECT_FALSE(data->empty());

    auto result = PyPickler::unpickle(*data);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_TRUE(result->is_int());
    EXPECT_EQ(PyLong_AsLong(result->get()), 42);
}

TEST_F(PyPicklerTest, PickleUnpickleString)
{
    auto py_str = py_convert::to_python(std::string_view{"hello atlas"});
    auto data = PyPickler::pickle(py_str.get());
    ASSERT_TRUE(data.has_value());

    auto result = PyPickler::unpickle(*data);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_string());

    auto cpp_str = py_convert::from_python<std::string>(result->get());
    ASSERT_TRUE(cpp_str.has_value());
    EXPECT_EQ(*cpp_str, "hello atlas");
}

TEST_F(PyPicklerTest, PickleUnpickleDict)
{
    // Create a dict in Python
    (void)PyInterpreter::exec("test_dict = {'a': 1, 'b': [2, 3]}");
    auto main = PyInterpreter::import("__main__");
    auto dict = main->get_attr("test_dict");
    ASSERT_TRUE(dict.is_dict());

    auto data = PyPickler::pickle(dict.get());
    ASSERT_TRUE(data.has_value());

    auto result = PyPickler::unpickle(*data);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_dict());
}

TEST_F(PyPicklerTest, PickleUnpickleList)
{
    auto list = PyObjectPtr(PyList_New(3));
    PyList_SetItem(list.get(), 0, PyLong_FromLong(10));
    PyList_SetItem(list.get(), 1, PyLong_FromLong(20));
    PyList_SetItem(list.get(), 2, PyLong_FromLong(30));

    auto data = PyPickler::pickle(list.get());
    ASSERT_TRUE(data.has_value());

    auto result = PyPickler::unpickle(*data);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_list());
    EXPECT_EQ(PyList_Size(result->get()), 3);
}

TEST_F(PyPicklerTest, PickleNoneSucceeds)
{
    auto data = PyPickler::pickle(Py_None);
    ASSERT_TRUE(data.has_value());

    auto result = PyPickler::unpickle(*data);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_none());
}

TEST_F(PyPicklerTest, UnpickleInvalidDataFails)
{
    std::vector<std::byte> garbage = {std::byte{0xFF}, std::byte{0xFE}};
    auto result = PyPickler::unpickle(garbage);
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Review fix: double-init is safe (returns immediately)
// ============================================================================

TEST_F(PyPicklerTest, DoubleInitializeIsSafe)
{
    // Already initialized in SetUp — second call should succeed silently
    auto result = PyPickler::initialize();
    EXPECT_TRUE(result.has_value());
}

// ============================================================================
// Review fix: pickle/unpickle with complex nested structure
// ============================================================================

TEST_F(PyPicklerTest, PickleUnpickleNestedStructure)
{
    (void)PyInterpreter::exec(
        "nested = {'key': [1, 2.5, 'text', None, True], 'count': 42}");
    auto main = PyInterpreter::import("__main__");
    auto nested = main->get_attr("nested");
    ASSERT_TRUE(nested.is_dict());

    auto data = PyPickler::pickle(nested.get());
    ASSERT_TRUE(data.has_value());

    auto result = PyPickler::unpickle(*data);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_dict());

    // Verify round-trip preserves structure via Python assertion
    // Store unpickled result back in __main__ and compare
    auto set_result = main->set_attr("unpickled", *result);
    EXPECT_TRUE(set_result.has_value());

    auto verify = PyInterpreter::exec("assert unpickled == nested");
    EXPECT_TRUE(verify.has_value()) << verify.error().message();
}
