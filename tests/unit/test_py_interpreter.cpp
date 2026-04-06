#include <gtest/gtest.h>
#include "pyscript/py_interpreter.hpp"

#include <thread>

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

// ============================================================================
// Review fix #1: Double initialize returns AlreadyExists (atomic flag)
// ============================================================================

TEST_F(PyInterpreterTest, DoubleInitializeReturnsAlreadyExists)
{
    // Interpreter is already initialized by SetUp
    EXPECT_TRUE(PyInterpreter::is_initialized());

    // Second initialize should return AlreadyExists, not crash
    auto result = PyInterpreter::initialize();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
}

// ============================================================================
// Review fix #1: is_initialized is safe to call from another thread
// ============================================================================

TEST_F(PyInterpreterTest, IsInitializedThreadSafe)
{
    std::atomic<bool> result{false};
    std::thread worker([&]()
    {
        result = PyInterpreter::is_initialized();
    });
    worker.join();
    EXPECT_TRUE(result.load());
}

// ============================================================================
// Review fix #5: exec with runtime error includes exception details
// ============================================================================

TEST_F(PyInterpreterTest, ExecRuntimeErrorReturnsScriptError)
{
    // PyRun_SimpleString prints and clears exceptions internally,
    // so we can only verify that exec returns an error code.
    auto result = PyInterpreter::exec("raise ValueError('test error 42')");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::ScriptError);

    // No lingering Python exception after exec
    EXPECT_FALSE(PyErr_Occurred());
}

// ============================================================================
// Review fix #5: import error includes module name or exception details
// ============================================================================

TEST_F(PyInterpreterTest, ImportErrorHasDetails)
{
    auto result = PyInterpreter::import("nonexistent_xyz_module");
    EXPECT_FALSE(result.has_value());
    auto msg = std::string(result.error().message());
    EXPECT_TRUE(msg.find("nonexistent_xyz_module") != std::string::npos
             || msg.find("ModuleNotFoundError") != std::string::npos
             || msg.find("No module named") != std::string::npos)
        << "Import error should be descriptive, got: " << msg;
}
