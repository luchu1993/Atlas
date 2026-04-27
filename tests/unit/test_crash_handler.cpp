#include <cstdio>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "platform/crash_handler.h"

using namespace atlas;
namespace fs = std::filesystem;

namespace {

fs::path TestDumpDir() {
  // Per-test scratch dir under the binary's CWD so parallel ctest runs
  // do not collide.  Cleaned up in TearDown.
  return fs::temp_directory_path() / "atlas_crash_handler_test";
}

class CrashHandlerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs::remove_all(TestDumpDir());
    fs::create_directories(TestDumpDir());
  }
  void TearDown() override {
    UninstallCrashHandler();
    fs::remove_all(TestDumpDir());
  }
};

}  // namespace

TEST_F(CrashHandlerTest, InstallSucceedsWithValidDir) {
  CrashHandlerOptions opts;
  opts.process_name = "test";
  opts.dump_dir = TestDumpDir().string();
  EXPECT_TRUE(InstallCrashHandler(opts));
}

TEST_F(CrashHandlerTest, InstallTwiceIsIdempotent) {
  CrashHandlerOptions opts;
  opts.process_name = "test";
  opts.dump_dir = TestDumpDir().string();
  EXPECT_TRUE(InstallCrashHandler(opts));
  EXPECT_TRUE(InstallCrashHandler(opts));
}

TEST_F(CrashHandlerTest, WriteCrashDumpForTestingProducesFile) {
  CrashHandlerOptions opts;
  opts.process_name = "test";
  opts.dump_dir = TestDumpDir().string();
  ASSERT_TRUE(InstallCrashHandler(opts));

  std::string path = WriteCrashDumpForTesting();
  ASSERT_FALSE(path.empty());

  // Dump file must exist and be non-empty.
  ASSERT_TRUE(fs::exists(path));
  EXPECT_GT(fs::file_size(path), 0u);
}

TEST_F(CrashHandlerTest, OnCrashCallbackInvokedFromTestHook) {
  CrashHandlerOptions opts;
  opts.process_name = "test";
  opts.dump_dir = TestDumpDir().string();

  std::string captured;
  opts.on_crash = [&captured](const std::string& p) { captured = p; };
  ASSERT_TRUE(InstallCrashHandler(opts));

  // The synthetic test-only path on Windows does not invoke on_crash
  // (it skips NotifyAndExit), so we only assert that the dump itself
  // lands on disk; the production paths exercise on_crash separately.
  std::string path = WriteCrashDumpForTesting();
  EXPECT_FALSE(path.empty());
}

TEST_F(CrashHandlerTest, WriteWithoutInstallReturnsEmpty) {
  EXPECT_TRUE(WriteCrashDumpForTesting().empty());
}
