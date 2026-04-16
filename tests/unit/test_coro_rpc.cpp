#include <atomic>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "coro/pending_rpc_registry.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "serialization/binary_stream.h"

using namespace atlas;

// ============================================================================
// Helper: build a fake payload with request_id as first 4 bytes (LE)
// ============================================================================

static auto make_payload(uint32_t request_id) -> std::vector<std::byte> {
  std::vector<std::byte> buf(sizeof(uint32_t));
  std::memcpy(buf.data(), &request_id, sizeof(uint32_t));
  return buf;
}

// ============================================================================
// PendingRpcRegistry tests
// ============================================================================

class PendingRpcRegistryTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test"};

  void SetUp() override { dispatcher_.SetMaxPollWait(Milliseconds(1)); }

  void drive_until(const std::atomic<bool>& done, Milliseconds timeout = Milliseconds(2000)) {
    auto deadline = Clock::now() + timeout;
    while (!done.load() && Clock::now() < deadline) dispatcher_.ProcessOnce();
  }
};

TEST_F(PendingRpcRegistryTest, BasicDispatch) {
  PendingRpcRegistry registry(dispatcher_);

  bool reply_received = false;
  registry.RegisterPending(
      42,   // reply message ID
      100,  // request_id
      [&](std::span<const std::byte>) { reply_received = true; }, [&](Error) {},
      Milliseconds(5000));

  EXPECT_EQ(registry.PendingCount(), 1u);

  auto payload = make_payload(100);
  bool consumed = registry.TryDispatch(42, payload);

  EXPECT_TRUE(consumed);
  EXPECT_TRUE(reply_received);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, NoMatchReturnsfalse) {
  PendingRpcRegistry registry(dispatcher_);

  registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {}, [](Error) {}, Milliseconds(5000));

  // Wrong message ID
  auto payload = make_payload(100);
  EXPECT_FALSE(registry.TryDispatch(99, payload));

  // Wrong request_id
  auto payload2 = make_payload(999);
  EXPECT_FALSE(registry.TryDispatch(42, payload2));

  EXPECT_EQ(registry.PendingCount(), 1u);
}

TEST_F(PendingRpcRegistryTest, Timeout) {
  PendingRpcRegistry registry(dispatcher_);

  std::atomic<bool> timed_out{false};
  ErrorCode error_code = ErrorCode::kNone;

  registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {},
      [&](Error err) {
        error_code = err.Code();
        timed_out = true;
      },
      Milliseconds(20));

  drive_until(timed_out, Milliseconds(500));

  EXPECT_TRUE(timed_out.load());
  EXPECT_EQ(error_code, ErrorCode::kTimeout);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, ManualCancel) {
  PendingRpcRegistry registry(dispatcher_);

  ErrorCode error_code = ErrorCode::kNone;
  auto handle = registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {}, [&](Error err) { error_code = err.Code(); },
      Milliseconds(5000));

  registry.Cancel(handle);

  EXPECT_EQ(error_code, ErrorCode::kCancelled);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, CancelAll) {
  PendingRpcRegistry registry(dispatcher_);

  int cancel_count = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    registry.RegisterPending(
        42, i, [](std::span<const std::byte>) {}, [&](Error) { ++cancel_count; },
        Milliseconds(5000));
  }

  EXPECT_EQ(registry.PendingCount(), 5u);
  registry.CancelAll();
  EXPECT_EQ(cancel_count, 5);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, PayloadTooSmallNotConsumed) {
  PendingRpcRegistry registry(dispatcher_);

  registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {}, [](Error) {}, Milliseconds(5000));

  // Payload smaller than 4 bytes — cannot extract request_id
  std::vector<std::byte> tiny(2);
  EXPECT_FALSE(registry.TryDispatch(42, tiny));
  EXPECT_EQ(registry.PendingCount(), 1u);
}

// ============================================================================
// InterfaceTable pre-dispatch hook test
// ============================================================================

TEST(InterfaceTable, PreDispatchHookInterceptsMessage) {
  InterfaceTable table;

  bool hook_called = false;
  MessageID intercepted_id = 0;

  table.SetPreDispatchHook([&](MessageID id, std::span<const std::byte> payload) -> bool {
    hook_called = true;
    intercepted_id = id;
    return true;  // consume the message
  });

  // Build a fake message
  BinaryWriter writer;
  writer.Write<uint32_t>(12345);  // request_id
  BinaryReader reader(writer.data());

  Address addr;
  auto result = table.Dispatch(addr, nullptr, 42, reader);

  EXPECT_TRUE(result.HasValue());
  EXPECT_TRUE(hook_called);
  EXPECT_EQ(intercepted_id, 42);
}

TEST(InterfaceTable, PreDispatchHookPassThrough) {
  InterfaceTable table;

  table.SetPreDispatchHook([](MessageID, std::span<const std::byte>) -> bool {
    return false;  // do not consume
  });

  BinaryWriter writer;
  writer.Write<uint32_t>(0);
  BinaryReader reader(writer.data());

  Address addr;
  // No handler registered for this ID — should fall through to NotFound
  auto result = table.Dispatch(addr, nullptr, 42, reader);
  EXPECT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kNotFound);
}
