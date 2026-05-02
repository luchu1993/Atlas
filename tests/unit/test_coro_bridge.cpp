#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "clrscript/coro_bridge.h"
#include "coro/pending_rpc_registry.h"
#include "network/event_dispatcher.h"

namespace {

struct CompletionRecord {
  intptr_t handle{0};
  int32_t status{-1};
  std::vector<uint8_t> payload;
};

// Module-static so the function-pointer signature stays plain C and matches
// the CoroOnRpcCompleteFn typedef.
inline std::vector<CompletionRecord>& Records() {
  static std::vector<CompletionRecord> records;
  return records;
}

extern "C" void RecordCompletion(intptr_t handle, int32_t status, const uint8_t* payload,
                                 int32_t len) {
  CompletionRecord rec{handle, status, {}};
  if (payload && len > 0) rec.payload.assign(payload, payload + len);
  Records().push_back(std::move(rec));
}

auto MakePayload(uint32_t request_id) -> std::vector<std::byte> {
  std::vector<std::byte> buf(sizeof(uint32_t));
  std::memcpy(buf.data(), &request_id, sizeof(uint32_t));
  return buf;
}

class CoroBridgeTest : public ::testing::Test {
 protected:
  atlas::EventDispatcher dispatcher_{"bridge"};
  atlas::PendingRpcRegistry registry_{dispatcher_};

  void SetUp() override {
    Records().clear();
    dispatcher_.SetMaxPollWait(atlas::Milliseconds(1));
  }
};

}  // namespace

TEST_F(CoroBridgeTest, RegisterAndDispatchSuccess) {
  auto handle = atlas::coro_bridge::RegisterPending(registry_, &RecordCompletion,
                                                    /*reply_id=*/42, /*request_id=*/100,
                                                    /*timeout_ms=*/5000,
                                                    /*managed_handle=*/0xDEADBEEF);
  EXPECT_NE(handle, 0u);
  EXPECT_EQ(registry_.PendingCount(), 1u);

  auto payload = MakePayload(100);
  EXPECT_TRUE(registry_.TryDispatch(42, payload));

  ASSERT_EQ(Records().size(), 1u);
  EXPECT_EQ(Records()[0].handle, intptr_t{0xDEADBEEF});
  EXPECT_EQ(Records()[0].status, 0);
  EXPECT_EQ(Records()[0].payload.size(), payload.size());
}

TEST_F(CoroBridgeTest, CancelPendingFiresCancelledStatus) {
  auto handle = atlas::coro_bridge::RegisterPending(registry_, &RecordCompletion, 42, 200, 5000,
                                                    0x1234);
  atlas::coro_bridge::CancelPending(registry_, handle);

  ASSERT_EQ(Records().size(), 1u);
  EXPECT_EQ(Records()[0].handle, intptr_t{0x1234});
  EXPECT_EQ(Records()[0].status, 2);  // Cancelled
  EXPECT_EQ(registry_.PendingCount(), 0u);
}

TEST_F(CoroBridgeTest, TimeoutFiresTimeoutStatus) {
  atlas::coro_bridge::RegisterPending(registry_, &RecordCompletion, 42, 300, 50, 0xABCD);

  auto deadline = atlas::Clock::now() + atlas::Milliseconds(2000);
  while (Records().empty() && atlas::Clock::now() < deadline) dispatcher_.ProcessOnce();

  ASSERT_EQ(Records().size(), 1u);
  EXPECT_EQ(Records()[0].handle, intptr_t{0xABCD});
  EXPECT_EQ(Records()[0].status, 1);  // Timeout
}

TEST_F(CoroBridgeTest, NullCallbackReturnsZero) {
  auto handle = atlas::coro_bridge::RegisterPending(registry_, /*on_complete=*/nullptr, 42, 1, 5000,
                                                    0x1);
  EXPECT_EQ(handle, 0u);
  EXPECT_EQ(registry_.PendingCount(), 0u);
}

TEST_F(CoroBridgeTest, HandleRoundTripsReplyAndRequestId) {
  // Two registrations with different (reply, req) pairs cancel independently.
  auto h1 = atlas::coro_bridge::RegisterPending(registry_, &RecordCompletion, 42, 1, 5000, 0x10);
  auto h2 = atlas::coro_bridge::RegisterPending(registry_, &RecordCompletion, 42, 2, 5000, 0x20);
  EXPECT_NE(h1, h2);

  atlas::coro_bridge::CancelPending(registry_, h1);
  ASSERT_EQ(Records().size(), 1u);
  EXPECT_EQ(Records()[0].handle, intptr_t{0x10});

  atlas::coro_bridge::CancelPending(registry_, h2);
  ASSERT_EQ(Records().size(), 2u);
  EXPECT_EQ(Records()[1].handle, intptr_t{0x20});
}

TEST_F(CoroBridgeTest, DispatchAfterCancelDoesNotFire) {
  auto handle = atlas::coro_bridge::RegisterPending(registry_, &RecordCompletion, 42, 400, 5000,
                                                    0xF1);
  atlas::coro_bridge::CancelPending(registry_, handle);
  Records().clear();

  auto payload = MakePayload(400);
  EXPECT_FALSE(registry_.TryDispatch(42, payload));
  EXPECT_TRUE(Records().empty());
}
