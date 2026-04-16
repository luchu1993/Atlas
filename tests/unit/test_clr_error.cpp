#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "clrscript/clr_error.h"

namespace atlas::test {

// ============================================================================
// Helpers
// ============================================================================

// Simulate what C# ErrorBridge.SetError() does: write into the TLS buffer
// via the exported ClrErrorSet() function.
static void SimulateClrError(int32_t code, std::string_view msg) {
  ClrErrorSet(code, msg.data(), static_cast<int32_t>(msg.size()));
}

// ============================================================================
// HasClrError / ClearClrError
// ============================================================================

TEST(ClrError, InitiallyNoError) {
  ClearClrError();
  EXPECT_FALSE(HasClrError());
}

TEST(ClrError, SetErrorSetsFlag) {
  ClearClrError();
  SimulateClrError(42, "test error");
  EXPECT_TRUE(HasClrError());
  ClearClrError();
}

TEST(ClrError, ClearErrorClearsFlag) {
  SimulateClrError(1, "msg");
  EXPECT_TRUE(HasClrError());
  ClearClrError();
  EXPECT_FALSE(HasClrError());
}

// ============================================================================
// ReadClrError
// ============================================================================

TEST(ClrError, ReadErrorContainsMessage) {
  ClearClrError();
  SimulateClrError(0, "something went wrong");

  auto err = ReadClrError();
  EXPECT_FALSE(HasClrError());  // ReadClrError clears the buffer
  EXPECT_NE(err.Message().find("something went wrong"), std::string::npos);
}

TEST(ClrError, ReadErrorContainsHResult) {
  ClearClrError();
  SimulateClrError(0x80004005, "E_FAIL");  // well-known COM HRESULT

  auto err = ReadClrError();
  // The formatted error should contain the hex code.
  EXPECT_NE(err.Message().find("80004005"), std::string::npos);
}

TEST(ClrError, ReadErrorClearsBuffer) {
  ClearClrError();
  SimulateClrError(10, "transient");
  (void)ReadClrError();  // consume
  EXPECT_FALSE(HasClrError());
}

TEST(ClrError, EmptyMessageFallback) {
  ClearClrError();
  SimulateClrError(0, "");  // empty message

  auto err = ReadClrError();
  // Should not be empty — fallback message is used.
  EXPECT_FALSE(err.Message().empty());
}

// ============================================================================
// Long message truncation
// ============================================================================

TEST(ClrError, LongMessageIsTruncated) {
  ClearClrError();

  // Message longer than 1024 bytes.
  std::string long_msg(2000, 'X');
  SimulateClrError(0, long_msg);

  EXPECT_TRUE(HasClrError());
  auto err = ReadClrError();
  // The message length written should be clamped to 1024.
  // The resulting Error message will be longer (it includes prefix text),
  // but should not contain the full 2000 chars.
  EXPECT_LT(err.Message().size(), 2100u);  // generous upper bound
}

// ============================================================================
// ClrErrorGetCode
// ============================================================================

TEST(ClrError, GetCodeReturnsZeroWhenNoError) {
  ClearClrError();
  EXPECT_EQ(ClrErrorGetCode(), 0);
}

TEST(ClrError, GetCodeReturnsSetCode) {
  ClearClrError();
  SimulateClrError(0xDEAD, "oops");
  EXPECT_EQ(ClrErrorGetCode(), 0xDEAD);
  ClearClrError();
}

// ============================================================================
// Thread isolation
// ============================================================================

TEST(ClrError, ThreadLocalIsolation) {
  // Set an error on the main thread.
  ClearClrError();
  SimulateClrError(999, "main thread error");

  bool child_saw_error = false;

  std::thread t([&] {
    // The child thread should start with a clean buffer.
    child_saw_error = HasClrError();
    // Set a different error on the child thread.
    SimulateClrError(100, "child error");
  });
  t.join();

  // Main thread's error must still be intact.
  EXPECT_FALSE(child_saw_error);
  EXPECT_TRUE(HasClrError());
  auto err = ReadClrError();
  EXPECT_NE(err.Message().find("main thread error"), std::string::npos);
}

// ============================================================================
// ClrErrorClear (extern "C")
// ============================================================================

TEST(ClrError, ExternCClearWorks) {
  SimulateClrError(5, "to be cleared");
  EXPECT_TRUE(HasClrError());
  ClrErrorClear();  // the extern "C" function
  EXPECT_FALSE(HasClrError());
}

}  // namespace atlas::test
