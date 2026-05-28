#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "platform/child_process.h"
#include "platform/platform_config.h"
#include "platform/process_launcher.h"

using namespace atlas;
using namespace std::chrono_literals;

namespace {

#if ATLAS_PLATFORM_WINDOWS
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
  opts.args = {"/c", "timeout /t " + std::to_string(seconds) + " /nobreak >NUL"};
  return opts;
}
auto MakeDetachedSleepOptions(int seconds) -> ProcessLaunchOptions {
  ProcessLaunchOptions opts;
  opts.exe = "C:\\Windows\\System32\\cmd.exe";
  opts.args = {"/c", "timeout /t " + std::to_string(seconds) + " /nobreak >NUL"};
  return opts;
}
auto MakeDetachedEchoOptions() -> ProcessLaunchOptions {
  ProcessLaunchOptions opts;
  opts.exe = "C:\\Windows\\System32\\cmd.exe";
  opts.args = {"/c", "echo detached-out && echo detached-err 1>&2"};
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
auto MakeDetachedSleepOptions(int seconds) -> ProcessLaunchOptions {
  ProcessLaunchOptions opts;
  opts.exe = "/bin/sh";
  opts.args = {"-c", "sleep " + std::to_string(seconds)};
  return opts;
}
auto MakeDetachedEchoOptions() -> ProcessLaunchOptions {
  ProcessLaunchOptions opts;
  opts.exe = "/bin/sh";
  opts.args = {"-c", "echo detached-out; echo detached-err >&2"};
  return opts;
}
#endif

auto ReadTextFile(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::in | std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

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
  EXPECT_TRUE(*code == 1 || *code == 143 || *code == 128 + 15 || *code == -1)
      << "unexpected exit code on kill: " << *code;
  EXPECT_FALSE(r->IsRunning());
}

TEST(ChildProcess, TerminateStopsDetachedChild) {
  auto launched = LaunchDetachedProcess(MakeDetachedSleepOptions(30));
  ASSERT_TRUE(launched.HasValue()) << launched.Error().Message();
  ASSERT_TRUE(launched->IsValid());
  ASSERT_TRUE(launched->IsAlive());

  EXPECT_TRUE(launched->Terminate());
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (launched->IsAlive() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_FALSE(launched->IsAlive());
}

TEST(ChildProcess, DetachedProcessRedirectsOutputToFile) {
  auto path = std::filesystem::temp_directory_path() /
              ("atlas_detached_output_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".log");
  std::error_code ec;
  std::filesystem::remove(path, ec);

  auto opts = MakeDetachedEchoOptions();
  opts.output_path = path;
  auto launched = LaunchDetachedProcess(std::move(opts));
  ASSERT_TRUE(launched.HasValue()) << launched.Error().Message();

  std::string content;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(path)) content = ReadTextFile(path);
    if (content.find("detached-out") != std::string::npos &&
        content.find("detached-err") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  if (content.find("detached-out") == std::string::npos ||
      content.find("detached-err") == std::string::npos) {
    (void)launched->Terminate();
  }
  EXPECT_NE(content.find("detached-out"), std::string::npos);
  EXPECT_NE(content.find("detached-err"), std::string::npos);
  std::filesystem::remove(path, ec);
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

  EXPECT_FALSE(r->IsRunning());
  EXPECT_EQ(r->TryReadStdoutLine(), std::nullopt);

  auto line = moved.WaitForStdoutLine(3s);
  ASSERT_TRUE(line.has_value());
  auto code = moved.Wait(2s);
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, 0);
}

#endif  // platform
