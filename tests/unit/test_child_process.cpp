#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "platform/child_process.h"
#include "platform/platform_config.h"

using namespace atlas;
using namespace std::chrono_literals;

namespace {

#if ATLAS_PLATFORM_WINDOWS
// cmd.exe is guaranteed on every supported Windows test machine; `/c` runs a
// single command then exits. We pick echo for stdout-only tests and a
// non-zero exit via `exit /b N` for status tests.
auto MakeEchoOptions(const std::string& msg) -> ChildProcess::Options {
  ChildProcess::Options opts;
  opts.exe = "C:\\Windows\\System32\\cmd.exe";
  opts.args = {"/c", "echo " + msg};
  return opts;
}
auto MakeExitOptions(int code) -> ChildProcess::Options {
  ChildProcess::Options opts;
  opts.exe = "C:\\Windows\\System32\\cmd.exe";
  opts.args = {"/c", "exit /b " + std::to_string(code)};
  return opts;
}
auto MakeSleepOptions(int seconds) -> ChildProcess::Options {
  ChildProcess::Options opts;
  opts.exe = "C:\\Windows\\System32\\cmd.exe";
  // `timeout /t N /nobreak` blocks for ~N seconds; redirecting >NUL
  // keeps its own banner out of our stdout parse.
  opts.args = {"/c", "timeout /t " + std::to_string(seconds) + " /nobreak >NUL"};
  return opts;
}
#elif ATLAS_PLATFORM_LINUX
auto MakeEchoOptions(const std::string& msg) -> ChildProcess::Options {
  ChildProcess::Options opts;
  opts.exe = "/bin/sh";
  opts.args = {"-c", "echo " + msg};
  return opts;
}
auto MakeExitOptions(int code) -> ChildProcess::Options {
  ChildProcess::Options opts;
  opts.exe = "/bin/sh";
  opts.args = {"-c", "exit " + std::to_string(code)};
  return opts;
}
auto MakeSleepOptions(int seconds) -> ChildProcess::Options {
  ChildProcess::Options opts;
  opts.exe = "/bin/sh";
  opts.args = {"-c", "sleep " + std::to_string(seconds)};
  return opts;
}
#endif

}  // namespace

#if ATLAS_PLATFORM_WINDOWS || ATLAS_PLATFORM_LINUX

TEST(ChildProcess, CapturesSingleLineStdout) {
  auto r = ChildProcess::Start(MakeEchoOptions("hello-atlas"));
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();

  auto line = r->WaitForStdoutLine(3s);
  ASSERT_TRUE(line.has_value()) << "timed out waiting for echo line";
  EXPECT_EQ(*line, "hello-atlas");

  auto code = r->Wait(2s);
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 0);
}

TEST(ChildProcess, NonZeroExitCodeIsVisible) {
  auto r = ChildProcess::Start(MakeExitOptions(42));
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();

  auto code = r->Wait(2s);
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 42);
}

TEST(ChildProcess, KillTerminatesLongRunningChild) {
  auto r = ChildProcess::Start(MakeSleepOptions(30));
  ASSERT_TRUE(r.HasValue()) << r.Error().Message();

  EXPECT_TRUE(r->IsRunning());
  r->Kill();

  auto code = r->Wait(3s);
  ASSERT_TRUE(code.has_value()) << "Kill did not reap the child within 3s";
  // Platforms report this differently:
  //   * Windows: exit code is the 1 we passed to TerminateProcess.
  //   * Linux: the process is signalled (SIGTERM) → 128+15 = 143.
  // Accept either — the assertion we care about is "terminated, reaped".
  EXPECT_TRUE(*code == 1 || *code == 143 || *code == 128 + 15 || *code == -1)
      << "unexpected exit code on kill: " << *code;
  EXPECT_FALSE(r->IsRunning());
}

TEST(ChildProcess, InvalidExePathReturnsError) {
  ChildProcess::Options opts;
  opts.exe = "/no/such/atlas_test_binary_xyzzy";
  auto r = ChildProcess::Start(std::move(opts));
  EXPECT_FALSE(r.HasValue());
}

TEST(ChildProcess, MovedFromBecomesInert) {
  auto r = ChildProcess::Start(MakeEchoOptions("move-test"));
  ASSERT_TRUE(r.HasValue());
  ChildProcess moved(std::move(*r));

  // Original handle must be inert after move.
  EXPECT_FALSE(r->IsRunning());
  EXPECT_EQ(r->TryReadStdoutLine(), std::nullopt);

  // Moved-to handle inherits ownership.
  auto line = moved.WaitForStdoutLine(3s);
  ASSERT_TRUE(line.has_value());
  auto code = moved.Wait(2s);
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 0);
}

#endif  // platform
