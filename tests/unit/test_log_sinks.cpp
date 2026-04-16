#include <filesystem>
#include <fstream>
#include <source_location>
#include <string>

#include <gtest/gtest.h>

#include "foundation/log_sinks.h"

using namespace atlas;
namespace stdfs = std::filesystem;

// ============================================================================
// ConsoleSink
// ============================================================================

TEST(ConsoleSink, WriteDoesNotCrash) {
  ConsoleSink sink;
  auto loc = std::source_location::current();
  EXPECT_NO_THROW(sink.Write(LogLevel::kInfo, "", "info message", loc));
  EXPECT_NO_THROW(sink.Write(LogLevel::kWarning, "net", "warning message", loc));
  EXPECT_NO_THROW(sink.Write(LogLevel::kError, "", "error message", loc));
  EXPECT_NO_THROW(sink.Write(LogLevel::kCritical, "sys", "critical message", loc));
}

TEST(ConsoleSink, FlushDoesNotCrash) {
  ConsoleSink sink;
  EXPECT_NO_THROW(sink.Flush());
}

TEST(ConsoleSink, AllLevelsWriteWithoutCrash) {
  ConsoleSink sink;
  auto loc = std::source_location::current();
  for (auto level : {LogLevel::kTrace, LogLevel::kDebug, LogLevel::kInfo, LogLevel::kWarning,
                     LogLevel::kError, LogLevel::kCritical}) {
    EXPECT_NO_THROW(sink.Write(level, "test", "msg", loc));
  }
}

TEST(ConsoleSink, EmptyAndNonEmptyCategory) {
  ConsoleSink sink;
  auto loc = std::source_location::current();
  // Empty category: no category field in output
  EXPECT_NO_THROW(sink.Write(LogLevel::kInfo, "", "no category", loc));
  // Non-empty category: category field included
  EXPECT_NO_THROW(sink.Write(LogLevel::kInfo, "mycat", "with category", loc));
}

// ============================================================================
// FileSink
// ============================================================================

class FileSinkTest : public ::testing::Test {
 protected:
  stdfs::path log_path_;

  void SetUp() override {
    log_path_ = stdfs::temp_directory_path() / "atlas_test_filesink.log";
    // Remove any leftover from a previous run
    std::error_code ec;
    stdfs::remove(log_path_, ec);
  }

  void TearDown() override {
    std::error_code ec;
    stdfs::remove(log_path_, ec);
  }

  // Read the file back as a string
  auto read_log() -> std::string {
    std::ifstream f(log_path_);
    if (!f) {
      return {};
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return content;
  }
};

TEST_F(FileSinkTest, WriteAndFlushProducesFile) {
  {
    FileSink sink(log_path_);
    auto loc = std::source_location::current();
    sink.Write(LogLevel::kInfo, "", "hello from file sink", loc);
    sink.Flush();
  }  // sink destructor closes file

  ASSERT_TRUE(stdfs::exists(log_path_));
  auto content = read_log();
  EXPECT_NE(content.find("hello from file sink"), std::string::npos);
}

TEST_F(FileSinkTest, LevelAppearsInOutput) {
  {
    FileSink sink(log_path_);
    auto loc = std::source_location::current();
    sink.Write(LogLevel::kWarning, "", "watch out", loc);
    sink.Flush();
  }

  auto content = read_log();
  EXPECT_NE(content.find("WARNING"), std::string::npos);
  EXPECT_NE(content.find("watch out"), std::string::npos);
}

TEST_F(FileSinkTest, CategoryAppearsInOutput) {
  {
    FileSink sink(log_path_);
    auto loc = std::source_location::current();
    sink.Write(LogLevel::kError, "network", "connection failed", loc);
    sink.Flush();
  }

  auto content = read_log();
  EXPECT_NE(content.find("network"), std::string::npos);
  EXPECT_NE(content.find("connection failed"), std::string::npos);
}

TEST_F(FileSinkTest, MultipleWritesAllAppear) {
  {
    FileSink sink(log_path_);
    auto loc = std::source_location::current();
    sink.Write(LogLevel::kInfo, "", "line one", loc);
    sink.Write(LogLevel::kInfo, "", "line two", loc);
    sink.Write(LogLevel::kInfo, "", "line three", loc);
    sink.Flush();
  }

  auto content = read_log();
  EXPECT_NE(content.find("line one"), std::string::npos);
  EXPECT_NE(content.find("line two"), std::string::npos);
  EXPECT_NE(content.find("line three"), std::string::npos);
}

TEST_F(FileSinkTest, AppendMode) {
  // Write in two separate FileSink lifetimes — file should contain both
  auto loc = std::source_location::current();
  {
    FileSink sink(log_path_);
    sink.Write(LogLevel::kInfo, "", "first session", loc);
    sink.Flush();
  }
  {
    FileSink sink(log_path_);
    sink.Write(LogLevel::kInfo, "", "second session", loc);
    sink.Flush();
  }

  auto content = read_log();
  EXPECT_NE(content.find("first session"), std::string::npos);
  EXPECT_NE(content.find("second session"), std::string::npos);
}

TEST_F(FileSinkTest, WriteToInvalidPathDoesNotCrash) {
  // Writing to a path that cannot be opened should not crash
  stdfs::path bad_path = "/nonexistent_dir_atlas_test/log.txt";
  EXPECT_NO_THROW({
    FileSink sink(bad_path);
    auto loc = std::source_location::current();
    sink.Write(LogLevel::kInfo, "", "should not crash", loc);
    sink.Flush();
  });
}

// ============================================================================
// Integration: FileSink via Logger
// ============================================================================

TEST_F(FileSinkTest, UsedViaLogger) {
  auto& logger = Logger::Instance();
  logger.ClearSinks();
  logger.SetLevel(LogLevel::kTrace);

  auto file_sink = std::make_shared<FileSink>(log_path_);
  logger.AddSink(file_sink);

  ATLAS_LOG_WARNING("test via logger to file");

  logger.Flush();
  logger.ClearSinks();

  auto content = read_log();
  EXPECT_NE(content.find("test via logger to file"), std::string::npos);
}
