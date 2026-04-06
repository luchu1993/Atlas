#include <gtest/gtest.h>
#include "pyscript/py_interpreter.hpp"

using namespace atlas;

class PyInterpreterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!PyInterpreter::is_initialized())
        {
            auto result = PyInterpreter::initialize();
            ASSERT_TRUE(result.has_value()) << result.error().message();
        }
    }

    // Note: Don't finalize in TearDown -- other tests may need the interpreter
};

TEST_F(PyInterpreterTest, InitializationSucceeds)
{
    EXPECT_TRUE(PyInterpreter::is_initialized());
}

TEST_F(PyInterpreterTest, VersionNotEmpty)
{
    auto ver = PyInterpreter::version();
    EXPECT_FALSE(ver.empty());
    EXPECT_TRUE(ver.find("3.") != std::string_view::npos);
}

TEST_F(PyInterpreterTest, ExecSimpleCode)
{
    auto result = PyInterpreter::exec("x = 1 + 1");
    EXPECT_TRUE(result.has_value()) << result.error().message();
}

TEST_F(PyInterpreterTest, ExecSyntaxErrorReturnsError)
{
    auto result = PyInterpreter::exec("if if if");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PyInterpreterTest, ImportSysSucceeds)
{
    auto result = PyInterpreter::import("sys");
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_TRUE(result->is_callable() == false);  // module, not callable
    EXPECT_TRUE(static_cast<bool>(*result));  // not null
}

TEST_F(PyInterpreterTest, ImportNonexistentFails)
{
    auto result = PyInterpreter::import("nonexistent_module_12345");
    EXPECT_FALSE(result.has_value());
}

TEST_F(PyInterpreterTest, AddSysPath)
{
    auto result = PyInterpreter::add_sys_path(".");
    EXPECT_TRUE(result.has_value());
}
