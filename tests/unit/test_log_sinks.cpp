#include "foundation/log_sinks.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <source_location>
#include <string>

using namespace atlas;
namespace stdfs = std::filesystem;

// ============================================================================
// ConsoleSink
// ============================================================================

TEST(ConsoleSink, WriteDoesNotCrash)
{
    ConsoleSink sink;
    auto loc = std::source_location::current();
    EXPECT_NO_THROW(sink.write(LogLevel::Info, "", "info message", loc));
    EXPECT_NO_THROW(sink.write(LogLevel::Warning, "net", "warning message", loc));
    EXPECT_NO_THROW(sink.write(LogLevel::Error, "", "error message", loc));
    EXPECT_NO_THROW(sink.write(LogLevel::Critical, "sys", "critical message", loc));
}

TEST(ConsoleSink, FlushDoesNotCrash)
{
    ConsoleSink sink;
    EXPECT_NO_THROW(sink.flush());
}

TEST(ConsoleSink, AllLevelsWriteWithoutCrash)
{
    ConsoleSink sink;
    auto loc = std::source_location::current();
    for (auto level : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warning,
                       LogLevel::Error, LogLevel::Critical})
    {
        EXPECT_NO_THROW(sink.write(level, "test", "msg", loc));
    }
}

TEST(ConsoleSink, EmptyAndNonEmptyCategory)
{
    ConsoleSink sink;
    auto loc = std::source_location::current();
    // Empty category: no category field in output
    EXPECT_NO_THROW(sink.write(LogLevel::Info, "", "no category", loc));
    // Non-empty category: category field included
    EXPECT_NO_THROW(sink.write(LogLevel::Info, "mycat", "with category", loc));
}

// ============================================================================
// FileSink
// ============================================================================

class FileSinkTest : public ::testing::Test
{
protected:
    stdfs::path log_path_;

    void SetUp() override
    {
        log_path_ = stdfs::temp_directory_path() / "atlas_test_filesink.log";
        // Remove any leftover from a previous run
        std::error_code ec;
        stdfs::remove(log_path_, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        stdfs::remove(log_path_, ec);
    }

    // Read the file back as a string
    auto read_log() -> std::string
    {
        std::ifstream f(log_path_);
        if (!f)
        {
            return {};
        }
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return content;
    }
};

TEST_F(FileSinkTest, WriteAndFlushProducesFile)
{
    {
        FileSink sink(log_path_);
        auto loc = std::source_location::current();
        sink.write(LogLevel::Info, "", "hello from file sink", loc);
        sink.flush();
    }  // sink destructor closes file

    ASSERT_TRUE(stdfs::exists(log_path_));
    auto content = read_log();
    EXPECT_NE(content.find("hello from file sink"), std::string::npos);
}

TEST_F(FileSinkTest, LevelAppearsInOutput)
{
    {
        FileSink sink(log_path_);
        auto loc = std::source_location::current();
        sink.write(LogLevel::Warning, "", "watch out", loc);
        sink.flush();
    }

    auto content = read_log();
    EXPECT_NE(content.find("WARNING"), std::string::npos);
    EXPECT_NE(content.find("watch out"), std::string::npos);
}

TEST_F(FileSinkTest, CategoryAppearsInOutput)
{
    {
        FileSink sink(log_path_);
        auto loc = std::source_location::current();
        sink.write(LogLevel::Error, "network", "connection failed", loc);
        sink.flush();
    }

    auto content = read_log();
    EXPECT_NE(content.find("network"), std::string::npos);
    EXPECT_NE(content.find("connection failed"), std::string::npos);
}

TEST_F(FileSinkTest, MultipleWritesAllAppear)
{
    {
        FileSink sink(log_path_);
        auto loc = std::source_location::current();
        sink.write(LogLevel::Info, "", "line one", loc);
        sink.write(LogLevel::Info, "", "line two", loc);
        sink.write(LogLevel::Info, "", "line three", loc);
        sink.flush();
    }

    auto content = read_log();
    EXPECT_NE(content.find("line one"), std::string::npos);
    EXPECT_NE(content.find("line two"), std::string::npos);
    EXPECT_NE(content.find("line three"), std::string::npos);
}

TEST_F(FileSinkTest, AppendMode)
{
    // Write in two separate FileSink lifetimes — file should contain both
    auto loc = std::source_location::current();
    {
        FileSink sink(log_path_);
        sink.write(LogLevel::Info, "", "first session", loc);
        sink.flush();
    }
    {
        FileSink sink(log_path_);
        sink.write(LogLevel::Info, "", "second session", loc);
        sink.flush();
    }

    auto content = read_log();
    EXPECT_NE(content.find("first session"), std::string::npos);
    EXPECT_NE(content.find("second session"), std::string::npos);
}

TEST_F(FileSinkTest, WriteToInvalidPathDoesNotCrash)
{
    // Writing to a path that cannot be opened should not crash
    stdfs::path bad_path = "/nonexistent_dir_atlas_test/log.txt";
    EXPECT_NO_THROW({
        FileSink sink(bad_path);
        auto loc = std::source_location::current();
        sink.write(LogLevel::Info, "", "should not crash", loc);
        sink.flush();
    });
}

// ============================================================================
// Integration: FileSink via Logger
// ============================================================================

TEST_F(FileSinkTest, UsedViaLogger)
{
    auto& logger = Logger::instance();
    logger.clear_sinks();
    logger.set_level(LogLevel::Trace);

    auto file_sink = std::make_shared<FileSink>(log_path_);
    logger.add_sink(file_sink);

    ATLAS_LOG_WARNING("test via logger to file");

    logger.flush();
    logger.clear_sinks();

    auto content = read_log();
    EXPECT_NE(content.find("test via logger to file"), std::string::npos);
}
