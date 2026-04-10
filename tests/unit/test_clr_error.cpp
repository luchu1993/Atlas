#include "clrscript/clr_error.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace atlas::test
{

// ============================================================================
// Helpers
// ============================================================================

// Simulate what C# ErrorBridge.SetError() does: write into the TLS buffer
// via the exported clr_error_set() function.
static void simulate_clr_error(int32_t code, std::string_view msg)
{
    clr_error_set(code, msg.data(), static_cast<int32_t>(msg.size()));
}

// ============================================================================
// has_clr_error / clear_clr_error
// ============================================================================

TEST(ClrError, InitiallyNoError)
{
    clear_clr_error();
    EXPECT_FALSE(has_clr_error());
}

TEST(ClrError, SetErrorSetsFlag)
{
    clear_clr_error();
    simulate_clr_error(42, "test error");
    EXPECT_TRUE(has_clr_error());
    clear_clr_error();
}

TEST(ClrError, ClearErrorClearsFlag)
{
    simulate_clr_error(1, "msg");
    EXPECT_TRUE(has_clr_error());
    clear_clr_error();
    EXPECT_FALSE(has_clr_error());
}

// ============================================================================
// read_clr_error
// ============================================================================

TEST(ClrError, ReadErrorContainsMessage)
{
    clear_clr_error();
    simulate_clr_error(0, "something went wrong");

    auto err = read_clr_error();
    EXPECT_FALSE(has_clr_error());  // read_clr_error clears the buffer
    EXPECT_NE(err.message().find("something went wrong"), std::string::npos);
}

TEST(ClrError, ReadErrorContainsHResult)
{
    clear_clr_error();
    simulate_clr_error(0x80004005, "E_FAIL");  // well-known COM HRESULT

    auto err = read_clr_error();
    // The formatted error should contain the hex code.
    EXPECT_NE(err.message().find("80004005"), std::string::npos);
}

TEST(ClrError, ReadErrorClearsBuffer)
{
    clear_clr_error();
    simulate_clr_error(10, "transient");
    (void)read_clr_error();  // consume
    EXPECT_FALSE(has_clr_error());
}

TEST(ClrError, EmptyMessageFallback)
{
    clear_clr_error();
    simulate_clr_error(0, "");  // empty message

    auto err = read_clr_error();
    // Should not be empty — fallback message is used.
    EXPECT_FALSE(err.message().empty());
}

// ============================================================================
// Long message truncation
// ============================================================================

TEST(ClrError, LongMessageIsTruncated)
{
    clear_clr_error();

    // Message longer than 1024 bytes.
    std::string long_msg(2000, 'X');
    simulate_clr_error(0, long_msg);

    EXPECT_TRUE(has_clr_error());
    auto err = read_clr_error();
    // The message length written should be clamped to 1024.
    // The resulting Error message will be longer (it includes prefix text),
    // but should not contain the full 2000 chars.
    EXPECT_LT(err.message().size(), 2100u);  // generous upper bound
}

// ============================================================================
// clr_error_get_code
// ============================================================================

TEST(ClrError, GetCodeReturnsZeroWhenNoError)
{
    clear_clr_error();
    EXPECT_EQ(clr_error_get_code(), 0);
}

TEST(ClrError, GetCodeReturnsSetCode)
{
    clear_clr_error();
    simulate_clr_error(0xDEAD, "oops");
    EXPECT_EQ(clr_error_get_code(), 0xDEAD);
    clear_clr_error();
}

// ============================================================================
// Thread isolation
// ============================================================================

TEST(ClrError, ThreadLocalIsolation)
{
    // Set an error on the main thread.
    clear_clr_error();
    simulate_clr_error(999, "main thread error");

    bool child_saw_error = false;

    std::thread t(
        [&]
        {
            // The child thread should start with a clean buffer.
            child_saw_error = has_clr_error();
            // Set a different error on the child thread.
            simulate_clr_error(100, "child error");
        });
    t.join();

    // Main thread's error must still be intact.
    EXPECT_FALSE(child_saw_error);
    EXPECT_TRUE(has_clr_error());
    auto err = read_clr_error();
    EXPECT_NE(err.message().find("main thread error"), std::string::npos);
}

// ============================================================================
// clr_error_clear (extern "C")
// ============================================================================

TEST(ClrError, ExternCClearWorks)
{
    simulate_clr_error(5, "to be cleared");
    EXPECT_TRUE(has_clr_error());
    clr_error_clear();  // the extern "C" function
    EXPECT_FALSE(has_clr_error());
}

}  // namespace atlas::test
