#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "foundation/log.h"

using namespace atlas;

class TestSink : public atlas::LogSink {
 public:
  std::vector<std::string> messages;
  void Write(atlas::LogLevel level, std::string_view category, std::string_view message,
             const std::source_location& loc) override {
    messages.emplace_back(message);
  }
  void Flush() override {}
};

class LogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& logger = Logger::Instance();
    logger.ClearSinks();
    logger.SetLevel(LogLevel::kTrace);
    sink_ = std::make_shared<TestSink>();
    logger.AddSink(sink_);
  }

  void TearDown() override {
    Logger::Instance().ClearSinks();
    Logger::Instance().SetLevel(LogLevel::kTrace);
  }

  std::shared_ptr<TestSink> sink_;
};

TEST(LogLevel, NameConversion) {
  EXPECT_EQ(LogLevelName(LogLevel::kTrace), "TRACE");
  EXPECT_EQ(LogLevelName(LogLevel::kInfo), "INFO");
  EXPECT_EQ(LogLevelName(LogLevel::kWarning), "WARNING");
  EXPECT_EQ(LogLevelName(LogLevel::kError), "ERROR");
  EXPECT_EQ(LogLevelName(LogLevel::kCritical), "CRITICAL");
}

TEST_F(LogTest, AddAndClearSinks) {
  auto& logger = Logger::Instance();
  ATLAS_LOG_INFO("test message");
  EXPECT_EQ(sink_->messages.size(), 1u);

  logger.ClearSinks();
  ATLAS_LOG_INFO("after clear");
  EXPECT_EQ(sink_->messages.size(), 1u);  // no new messages
}

TEST_F(LogTest, InfoReachesSink) {
  ATLAS_LOG_INFO("hello {}", 42);
  ASSERT_EQ(sink_->messages.size(), 1u);
  EXPECT_EQ(sink_->messages[0], "hello 42");
}

TEST_F(LogTest, LevelFiltering) {
  Logger::Instance().SetLevel(LogLevel::kWarning);
  ATLAS_LOG_INFO("should be filtered");
  EXPECT_TRUE(sink_->messages.empty());

  ATLAS_LOG_WARNING("should pass");
  EXPECT_EQ(sink_->messages.size(), 1u);
}

TEST_F(LogTest, LazyEvaluation) {
  // Set level to Warning so Info messages should not reach the sink
  Logger::Instance().SetLevel(LogLevel::kWarning);

  ATLAS_LOG_INFO("should not reach sink");
  EXPECT_TRUE(sink_->messages.empty());
}

// ============================================================================
// Review issue: Logger deadlock prevention (recursive logging)
// ============================================================================

class RecursiveSink : public atlas::LogSink {
 public:
  int call_count{0};
  void Write(atlas::LogLevel level, std::string_view category, std::string_view message,
             const std::source_location& loc) override {
    ++call_count;
    // In old code, this would deadlock because Logger::log() held the lock
    // while calling sink->write(). Now it copies sinks and releases the lock
    // before invoking, so this should be safe (but the recursive message
    // won't reach this sink since we're iterating a snapshot).
    if (call_count == 1) {
      // Try to log from within a sink callback
      atlas::Logger::Instance().Log(atlas::LogLevel::kWarning, "", "recursive log from sink");
    }
  }
  void Flush() override {}
};

TEST_F(LogTest, RecursiveLoggingDoesNotDeadlock) {
  auto& logger = Logger::Instance();
  logger.ClearSinks();
  auto recursive_sink = std::make_shared<RecursiveSink>();
  logger.AddSink(recursive_sink);

  // This should NOT deadlock
  ATLAS_LOG_INFO("trigger recursive");
  EXPECT_GE(recursive_sink->call_count, 1);
}

// ============================================================================
// Review issue: multiple sinks receive messages
// ============================================================================

TEST_F(LogTest, MultipleSinksReceiveMessages) {
  auto second_sink = std::make_shared<TestSink>();
  Logger::Instance().AddSink(second_sink);

  ATLAS_LOG_INFO("broadcast");
  EXPECT_EQ(sink_->messages.size(), 1u);
  EXPECT_EQ(second_sink->messages.size(), 1u);
  EXPECT_EQ(sink_->messages[0], "broadcast");
  EXPECT_EQ(second_sink->messages[0], "broadcast");
}

// ============================================================================
// Review issue #7: Logger snapshot isolation — sinks are snapshot-copied
// before invocation, so clear_sinks() during log() is safe.
// ============================================================================

class ClearingSink : public atlas::LogSink {
 public:
  int write_count{0};
  std::string last_message;

  void Write(atlas::LogLevel level, std::string_view category, std::string_view message,
             const std::source_location& loc) override {
    ++write_count;
    last_message = std::string(message);

    // Clear all sinks during the write callback.
    // This should NOT crash or deadlock because Logger copies the sink
    // list before invoking write(), so modifying the original list is safe.
    atlas::Logger::Instance().ClearSinks();
  }

  void Flush() override {}
};

TEST_F(LogTest, ClearSinksDuringLogDoesNotCrashOrDeadlock) {
  auto& logger = Logger::Instance();
  logger.ClearSinks();

  auto clearing_sink = std::make_shared<ClearingSink>();
  logger.AddSink(clearing_sink);

  // This should NOT crash or deadlock
  ATLAS_LOG_INFO("snapshot isolation test");

  // The sink should have received the message before clear_sinks() took effect
  EXPECT_EQ(clearing_sink->write_count, 1);
  EXPECT_EQ(clearing_sink->last_message, "snapshot isolation test");

  // After the log call, sinks should be cleared (the sink cleared them)
  // Logging again should not reach the sink
  ATLAS_LOG_INFO("after clear");
  EXPECT_EQ(clearing_sink->write_count, 1);  // no new writes
}
