#include <gtest/gtest.h>
#include "foundation/log.hpp"

#include <string>
#include <vector>

using namespace atlas;

class TestSink : public atlas::LogSink
{
public:
    std::vector<std::string> messages;
    void write(atlas::LogLevel level, std::string_view category,
               std::string_view message, const std::source_location& loc) override
    {
        messages.emplace_back(message);
    }
    void flush() override {}
};

class LogTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& logger = Logger::instance();
        logger.clear_sinks();
        logger.set_level(LogLevel::Trace);
        sink_ = std::make_shared<TestSink>();
        logger.add_sink(sink_);
    }

    void TearDown() override
    {
        Logger::instance().clear_sinks();
        Logger::instance().set_level(LogLevel::Trace);
    }

    std::shared_ptr<TestSink> sink_;
};

TEST(LogLevel, NameConversion)
{
    EXPECT_EQ(log_level_name(LogLevel::Trace), "TRACE");
    EXPECT_EQ(log_level_name(LogLevel::Info), "INFO");
    EXPECT_EQ(log_level_name(LogLevel::Warning), "WARNING");
    EXPECT_EQ(log_level_name(LogLevel::Error), "ERROR");
    EXPECT_EQ(log_level_name(LogLevel::Critical), "CRITICAL");
}

TEST_F(LogTest, AddAndClearSinks)
{
    auto& logger = Logger::instance();
    ATLAS_LOG_INFO("test message");
    EXPECT_EQ(sink_->messages.size(), 1u);

    logger.clear_sinks();
    ATLAS_LOG_INFO("after clear");
    EXPECT_EQ(sink_->messages.size(), 1u);  // no new messages
}

TEST_F(LogTest, InfoReachesSink)
{
    ATLAS_LOG_INFO("hello {}", 42);
    ASSERT_EQ(sink_->messages.size(), 1u);
    EXPECT_EQ(sink_->messages[0], "hello 42");
}

TEST_F(LogTest, LevelFiltering)
{
    Logger::instance().set_level(LogLevel::Warning);
    ATLAS_LOG_INFO("should be filtered");
    EXPECT_TRUE(sink_->messages.empty());

    ATLAS_LOG_WARNING("should pass");
    EXPECT_EQ(sink_->messages.size(), 1u);
}

TEST_F(LogTest, LazyEvaluation)
{
    // Set level to Warning so Info messages should not reach the sink
    Logger::instance().set_level(LogLevel::Warning);

    ATLAS_LOG_INFO("should not reach sink");
    EXPECT_TRUE(sink_->messages.empty());
}
