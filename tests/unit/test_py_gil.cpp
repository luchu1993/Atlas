#include <gtest/gtest.h>
#include "pyscript/py_interpreter.hpp"
#include "pyscript/py_gil.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace atlas;

class PyGilTest : public ::testing::Test
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

TEST_F(PyGilTest, GILGuardDoesNotDeadlock)
{
    // Main thread already holds GIL after initialize
    // GILGuard should acquire/release safely
    {
        GILGuard guard;
        // Can call Python API here
        auto py_val = PyObjectPtr(PyLong_FromLong(42));
        EXPECT_TRUE(py_val.is_int());
    }
    // After guard destroyed, GIL state should be restored
}

TEST_F(PyGilTest, GILReleaseAndReacquire)
{
    // Release GIL for "long C++ operation"
    {
        GILRelease release;
        // Cannot call Python API here (no GIL)
        // Simulate work
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // GIL re-acquired, can call Python API
    auto py_val = PyObjectPtr(PyLong_FromLong(99));
    EXPECT_TRUE(py_val.is_int());
}

TEST_F(PyGilTest, WorkerThreadWithGILGuard)
{
    std::atomic<bool> thread_success{false};

    // Release GIL so worker thread can acquire it
    {
        GILRelease main_release;

        std::thread worker([&]()
        {
            GILGuard guard;
            auto val = PyObjectPtr(PyLong_FromLong(123));
            thread_success = val.is_int();
        });
        worker.join();
    }

    EXPECT_TRUE(thread_success.load());
}
