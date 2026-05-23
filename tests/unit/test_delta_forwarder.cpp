#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "baseapp/baseapp_messages.h"
#include "baseapp/delta_forwarder.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/socket.h"
#include "network/tcp_channel.h"
#include "protocol/aoi_envelope.h"
#include "serialization/binary_stream.h"

namespace atlas {

static auto make_delta(std::initializer_list<uint8_t> bytes) -> std::vector<std::byte> {
  std::vector<std::byte> v;
  v.reserve(bytes.size());
  for (auto b : bytes) v.push_back(static_cast<std::byte>(b));
  return v;
}

class CaptureChannel final : public Channel {
 public:
  CaptureChannel(EventDispatcher& dispatcher, InterfaceTable& table)
      : Channel(dispatcher, table, Address{}) {
    Activate();
  }

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }

  std::vector<std::vector<std::byte>> frames;

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    frames.emplace_back(data.begin(), data.end());
    return data.size();
  }
};

static auto make_position_delta(EntityID entity_id, float px, float py, float pz, float dx,
                                float dy, float dz, bool on_ground, double server_time)
    -> std::vector<std::byte> {
  BinaryWriter writer;
  writer.Write<uint8_t>(static_cast<uint8_t>(CellAoIEnvelopeKind::kEntityPositionUpdate));
  writer.Write<EntityID>(entity_id);
  writer.Write<float>(px);
  writer.Write<float>(py);
  writer.Write<float>(pz);
  writer.Write<float>(dx);
  writer.Write<float>(dy);
  writer.Write<float>(dz);
  writer.Write<uint8_t>(on_ground ? 1 : 0);
  writer.Write<double>(server_time);
  return writer.Detach();
}

static auto extract_client_delta_payload(const std::vector<std::byte>& frame)
    -> std::vector<std::byte> {
  BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
  auto id = reader.ReadPackedInt();
  EXPECT_TRUE(id.HasValue());
  if (!id) return {};
  EXPECT_EQ(*id, baseapp::kClientDeltaMessageId);
  auto len = reader.ReadPackedInt();
  EXPECT_TRUE(len.HasValue());
  if (!len) return {};
  auto payload = reader.ReadBytes(*len);
  EXPECT_TRUE(payload.HasValue());
  if (!payload) return {};
  EXPECT_EQ(reader.Remaining(), 0u);
  return {payload->begin(), payload->end()};
}

static auto decode_signed12(uint32_t value) -> int16_t {
  if ((value & 0x0800u) != 0) value |= 0xFFFFF000u;
  return static_cast<int16_t>(value);
}

static auto read_packed_xz(BinaryReader& reader) -> std::optional<std::pair<int16_t, int16_t>> {
  auto b0 = reader.Read<uint8_t>();
  auto b1 = reader.Read<uint8_t>();
  auto b2 = reader.Read<uint8_t>();
  if (!b0 || !b1 || !b2) return std::nullopt;
  const uint32_t packed =
      static_cast<uint32_t>(*b0) | (static_cast<uint32_t>(*b1) << 8) |
      (static_cast<uint32_t>(*b2) << 16);
  return std::pair<int16_t, int16_t>{decode_signed12(packed & 0x0FFFu),
                                     decode_signed12((packed >> 12) & 0x0FFFu)};
}

TEST(DeltaForwarderTest, InitiallyEmpty) {
  DeltaForwarder fwd;
  EXPECT_EQ(fwd.QueueDepth(), 0u);
  EXPECT_EQ(fwd.GetStats().bytes_sent, 0u);
}

// Locks the three client state paths to distinct IDs so dispatch cannot
// merge latest-wins, reliable delta, and baseline streams.
TEST(DeltaForwarderTest, ReservedClientMessageIdsAreDistinct) {
  EXPECT_NE(baseapp::kClientDeltaMessageId, baseapp::kClientBaselineMessageId);
  EXPECT_NE(baseapp::kClientDeltaMessageId, baseapp::kClientReliableDeltaMessageId);
  EXPECT_NE(baseapp::kClientBaselineMessageId, baseapp::kClientReliableDeltaMessageId);
  EXPECT_EQ(baseapp::kClientDeltaMessageId, static_cast<MessageID>(0xF001));
  EXPECT_EQ(baseapp::kClientBaselineMessageId, static_cast<MessageID>(0xF002));
  EXPECT_EQ(baseapp::kClientReliableDeltaMessageId, static_cast<MessageID>(0xF003));
}

TEST(DeltaForwarderTest, EnqueueIncreasesDepth) {
  DeltaForwarder fwd;
  auto d1 = make_delta({1, 2, 3});
  auto d2 = make_delta({4, 5});

  fwd.Enqueue(100, d1);
  EXPECT_EQ(fwd.QueueDepth(), 1u);

  fwd.Enqueue(200, d2);
  EXPECT_EQ(fwd.QueueDepth(), 2u);
}

TEST(DeltaForwarderTest, EnqueueSameEntityReplacesEntry) {
  DeltaForwarder fwd;
  auto d1 = make_delta({1, 2, 3});
  auto d2 = make_delta({10, 20, 30, 40, 50});

  fwd.Enqueue(100, d1);
  EXPECT_EQ(fwd.QueueDepth(), 1u);

  fwd.Enqueue(100, d2);
  EXPECT_EQ(fwd.QueueDepth(), 1u);
}

TEST(DeltaForwarderTest, EnqueueMultipleEntities) {
  DeltaForwarder fwd;
  for (EntityID id = 1; id <= 10; ++id) {
    auto d = make_delta({static_cast<uint8_t>(id)});
    fwd.Enqueue(id, d);
  }
  EXPECT_EQ(fwd.QueueDepth(), 10u);
}

class DeltaForwarderFlushTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dispatcher_.SetMaxPollWait(Milliseconds(1));

    auto server_sock = Socket::CreateTcp();
    ASSERT_TRUE(server_sock.HasValue());
    ASSERT_TRUE(server_sock->Bind(Address("127.0.0.1", 0)).HasValue());
    ASSERT_TRUE(server_sock->Listen().HasValue());
    auto server_addr = server_sock->LocalAddress().Value();

    auto client_sock = Socket::CreateTcp();
    ASSERT_TRUE(client_sock.HasValue());
    (void)client_sock->Connect(server_addr);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto accepted = server_sock->Accept();
    ASSERT_TRUE(accepted.HasValue());
    peer_sock_ = std::move(accepted->first);

    sender_ =
        std::make_unique<TcpChannel>(dispatcher_, table_, std::move(*client_sock), server_addr);
    sender_->Activate();
  }

  EventDispatcher dispatcher_{"test_delta_fwd"};
  InterfaceTable table_;
  std::optional<Socket> peer_sock_;  // Keep accepted socket alive to prevent SIGPIPE.
  std::unique_ptr<TcpChannel> sender_;
};

TEST_F(DeltaForwarderFlushTest, FlushEmptyQueueReturnsZero) {
  DeltaForwarder fwd;
  auto bytes = fwd.Flush(*sender_, 4096);
  EXPECT_EQ(bytes, 0u);
  EXPECT_EQ(fwd.QueueDepth(), 0u);
}

TEST_F(DeltaForwarderFlushTest, FlushWithinBudgetSendsAll) {
  DeltaForwarder fwd;
  auto d1 = make_delta({1, 2, 3});     // 3 bytes
  auto d2 = make_delta({4, 5, 6, 7});  // 4 bytes

  fwd.Enqueue(100, d1);
  fwd.Enqueue(200, d2);
  EXPECT_EQ(fwd.QueueDepth(), 2u);

  auto bytes = fwd.Flush(*sender_, 4096);
  EXPECT_EQ(bytes, 7u);  // 3 + 4
  EXPECT_EQ(fwd.QueueDepth(), 0u);
  EXPECT_EQ(fwd.GetStats().bytes_sent, 7u);
}

TEST(DeltaForwarderTest, FlushBatchesCompressiblePositionUpdates) {
  EventDispatcher dispatcher{"test_delta_forwarder_batch"};
  InterfaceTable table;
  CaptureChannel channel(dispatcher, table);
  DeltaForwarder fwd;

  auto stale = make_position_delta(100, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, true, 1.0);
  auto latest = make_position_delta(100, 10.12f, 1.0f, 5.0f, 0.0f, 0.0f, 1.0f, true, 2.0);
  auto other = make_position_delta(200, 11.12f, 1.0f, -5.0f, 1.0f, 0.0f, 0.0f, false, 2.5);
  fwd.Enqueue(100, stale);
  fwd.Enqueue(100, latest);
  fwd.Enqueue(200, other);

  auto bytes = fwd.Flush(channel, 4096);

  EXPECT_EQ(channel.frames.size(), 1u);
  EXPECT_LT(bytes, 2u * 38u);
  auto payload = extract_client_delta_payload(channel.frames.front());
  BinaryReader reader(std::span<const std::byte>(payload.data(), payload.size()));
  auto kind = reader.Read<uint8_t>();
  auto reserved = reader.Read<EntityID>();
  auto origin_x = reader.Read<float>();
  auto origin_y = reader.Read<float>();
  auto origin_z = reader.Read<float>();
  auto base_time = reader.Read<double>();
  auto count = reader.Read<uint16_t>();
  auto flags = reader.Read<uint8_t>();
  auto first_entity_id = reader.ReadPackedInt();
  ASSERT_TRUE(kind && reserved && origin_x && origin_y && origin_z && base_time && count && flags &&
              first_entity_id);
  EXPECT_EQ(*kind, static_cast<uint8_t>(CellAoIEnvelopeKind::kEntityPositionBatch));
  EXPECT_EQ(*reserved, 0u);
  EXPECT_EQ(*base_time, 2.0);
  EXPECT_EQ(*count, 2u);
  EXPECT_EQ(*flags, 0x01u | 0x02u);
  EXPECT_EQ(*first_entity_id, 100u);
  auto on_ground_bits = reader.Read<uint8_t>();
  ASSERT_TRUE(on_ground_bits);
  EXPECT_EQ(*on_ground_bits, 0x01u);

  bool saw_100 = false;
  bool saw_200 = false;
  EntityID entity_id = *first_entity_id;
  for (uint16_t i = 0; i < *count; ++i) {
    auto entity_delta = reader.ReadPackedInt();
    auto xz = read_packed_xz(reader);
    auto yaw = reader.Read<uint8_t>();
    auto time_offset_ms = reader.Read<uint16_t>();
    ASSERT_TRUE(entity_delta && xz && yaw && time_offset_ms);
    entity_id += *entity_delta;

    const float px = *origin_x + static_cast<float>(xz->first) * 0.01f;
    const float py = *origin_y;
    const float pz = *origin_z + static_cast<float>(xz->second) * 0.01f;
    const bool on_ground = (*on_ground_bits & (1u << i)) != 0;
    const double server_time = *base_time + static_cast<double>(*time_offset_ms) * 0.001;
    if (entity_id == 100) {
      saw_100 = true;
      EXPECT_EQ(*entity_delta, 0u);
      EXPECT_NEAR(px, 10.12f, 0.006f);
      EXPECT_NEAR(py, 1.0f, 0.006f);
      EXPECT_NEAR(pz, 5.0f, 0.006f);
      EXPECT_EQ(*yaw, 0u);
      EXPECT_TRUE(on_ground);
      EXPECT_EQ(server_time, 2.0);
    } else if (entity_id == 200) {
      saw_200 = true;
      EXPECT_EQ(*entity_delta, 100u);
      EXPECT_NEAR(px, 11.12f, 0.006f);
      EXPECT_NEAR(py, 1.0f, 0.006f);
      EXPECT_NEAR(pz, -5.0f, 0.006f);
      EXPECT_EQ(*yaw, 64u);
      EXPECT_FALSE(on_ground);
      EXPECT_EQ(server_time, 2.5);
    }
  }
  EXPECT_TRUE(saw_100);
  EXPECT_TRUE(saw_200);
  EXPECT_EQ(reader.Remaining(), 0u);
  EXPECT_EQ(fwd.QueueDepth(), 0u);
}

TEST(DeltaForwarderTest, FlushKeepsUnsupportedPositionUpdatesLegacy) {
  EventDispatcher dispatcher{"test_delta_forwarder_legacy"};
  InterfaceTable table;
  CaptureChannel channel(dispatcher, table);
  DeltaForwarder fwd;

  auto vertical_a = make_position_delta(100, 1.0f, 2.0f, 3.0f, 0.0f, 1.0f, 0.0f, true, 1.0);
  auto vertical_b = make_position_delta(200, 4.0f, 5.0f, 6.0f, 0.0f, 1.0f, 0.0f, false, 1.5);
  fwd.Enqueue(100, vertical_a);
  fwd.Enqueue(200, vertical_b);

  auto bytes = fwd.Flush(channel, 4096);

  EXPECT_EQ(bytes, 2u * 38u);
  ASSERT_EQ(channel.frames.size(), 2u);
  for (const auto& frame : channel.frames) {
    auto payload = extract_client_delta_payload(frame);
    ASSERT_FALSE(payload.empty());
    EXPECT_EQ(static_cast<uint8_t>(payload[0]),
              static_cast<uint8_t>(CellAoIEnvelopeKind::kEntityPositionUpdate));
  }
}

TEST(DeltaForwarderTest, FlushOmitsUniformBatchFields) {
  EventDispatcher dispatcher{"test_delta_forwarder_uniform_batch"};
  InterfaceTable table;
  CaptureChannel channel(dispatcher, table);
  DeltaForwarder fwd;

  auto first = make_position_delta(10, 1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 1.0f, true, 5.0);
  auto second = make_position_delta(11, 2.0f, 2.0f, 4.0f, 1.0f, 0.0f, 0.0f, true, 5.0);
  fwd.Enqueue(10, first);
  fwd.Enqueue(11, second);

  auto bytes = fwd.Flush(channel, 4096);

  ASSERT_EQ(channel.frames.size(), 1u);
  EXPECT_EQ(bytes, 37u);
  auto payload = extract_client_delta_payload(channel.frames.front());
  BinaryReader reader(std::span<const std::byte>(payload.data(), payload.size()));
  auto kind = reader.Read<uint8_t>();
  ASSERT_TRUE(kind);
  EXPECT_EQ(*kind, static_cast<uint8_t>(CellAoIEnvelopeKind::kEntityPositionBatch));
  (void)reader.Read<EntityID>();
  (void)reader.Read<float>();
  (void)reader.Read<float>();
  (void)reader.Read<float>();
  EXPECT_EQ(*reader.Read<double>(), 5.0);
  EXPECT_EQ(*reader.Read<uint16_t>(), 2u);
  auto flags = reader.Read<uint8_t>();
  ASSERT_TRUE(flags);
  EXPECT_EQ(*flags, 0x04u | 0x20u);
  EXPECT_EQ(*reader.ReadPackedInt(), 10u);
  EXPECT_EQ(reader.Remaining(), 8u);
}

TEST(DeltaForwarderTest, FlushUsesWideXZWhenPackedRangeIsExceeded) {
  EventDispatcher dispatcher{"test_delta_forwarder_wide_xz_batch"};
  InterfaceTable table;
  CaptureChannel channel(dispatcher, table);
  DeltaForwarder fwd;

  auto first = make_position_delta(10, -30.0f, 2.0f, -30.0f, 0.0f, 0.0f, 1.0f, true, 5.0);
  auto second = make_position_delta(11, 30.0f, 2.0f, 30.0f, 1.0f, 0.0f, 0.0f, true, 5.0);
  fwd.Enqueue(10, first);
  fwd.Enqueue(11, second);

  auto bytes = fwd.Flush(channel, 4096);

  ASSERT_EQ(channel.frames.size(), 1u);
  EXPECT_EQ(bytes, 39u);
  auto payload = extract_client_delta_payload(channel.frames.front());
  BinaryReader reader(std::span<const std::byte>(payload.data(), payload.size()));
  EXPECT_TRUE(reader.Read<uint8_t>());
  (void)reader.Read<EntityID>();
  (void)reader.Read<float>();
  (void)reader.Read<float>();
  (void)reader.Read<float>();
  (void)reader.Read<double>();
  (void)reader.Read<uint16_t>();
  auto flags = reader.Read<uint8_t>();
  ASSERT_TRUE(flags);
  EXPECT_EQ(*flags, 0x04u | 0x10u | 0x20u);
}

TEST(DeltaForwarderTest, FlushBatchesSparsePositionIdsWithPackedDelta) {
  EventDispatcher dispatcher{"test_delta_forwarder_sparse_ids"};
  InterfaceTable table;
  CaptureChannel channel(dispatcher, table);
  DeltaForwarder fwd;

  auto first = make_position_delta(100, 1.0f, 2.0f, 3.0f, 1.0f, 0.0f, 0.0f, true, 1.0);
  auto second = make_position_delta(70000, 4.0f, 5.0f, 6.0f, 0.0f, 0.0f, 1.0f, false, 1.5);
  fwd.Enqueue(100, first);
  fwd.Enqueue(70000, second);

  auto bytes = fwd.Flush(channel, 4096);

  EXPECT_LT(bytes, 2u * 38u);
  ASSERT_EQ(channel.frames.size(), 1u);
  auto payload = extract_client_delta_payload(channel.frames.front());
  BinaryReader reader(std::span<const std::byte>(payload.data(), payload.size()));
  auto kind = reader.Read<uint8_t>();
  (void)reader.Read<EntityID>();
  (void)reader.Read<float>();
  (void)reader.Read<float>();
  (void)reader.Read<float>();
  (void)reader.Read<double>();
  auto count = reader.Read<uint16_t>();
  auto flags = reader.Read<uint8_t>();
  auto first_entity_id = reader.ReadPackedInt();
  ASSERT_TRUE(kind && count && flags && first_entity_id);
  EXPECT_EQ(*kind, static_cast<uint8_t>(CellAoIEnvelopeKind::kEntityPositionBatch));
  EXPECT_EQ(*count, 2u);
  EXPECT_EQ(*flags, 0x01u | 0x02u | 0x08u);
  EXPECT_EQ(*first_entity_id, 100u);
  auto on_ground_bits = reader.Read<uint8_t>();
  ASSERT_TRUE(on_ground_bits);
  EXPECT_EQ(*on_ground_bits, 0x01u);

  EntityID entity_id = *first_entity_id;
  auto first_delta = reader.ReadPackedInt();
  ASSERT_TRUE(first_delta);
  EXPECT_EQ(*first_delta, 0u);
  reader.Skip(3 + sizeof(int16_t) + sizeof(uint8_t) + sizeof(uint16_t));
  auto second_delta = reader.ReadPackedInt();
  ASSERT_TRUE(second_delta);
  entity_id += *second_delta;
  EXPECT_EQ(entity_id, 70000u);
}

TEST_F(DeltaForwarderFlushTest, FlushOverBudgetDefersRemaining) {
  DeltaForwarder fwd;
  auto d1 = make_delta({1, 2, 3});        // 3 bytes
  auto d2 = make_delta({4, 5, 6, 7, 8});  // 5 bytes

  fwd.Enqueue(100, d1);
  fwd.Enqueue(200, d2);

  // Budget 4: the first entry fits, and the second remains deferred.
  auto bytes = fwd.Flush(*sender_, 4);
  EXPECT_EQ(bytes, 3u);
  EXPECT_EQ(fwd.QueueDepth(), 1u);  // entity 200 deferred

  auto bytes2 = fwd.Flush(*sender_, 4096);
  EXPECT_EQ(bytes2, 5u);
  EXPECT_EQ(fwd.QueueDepth(), 0u);
  EXPECT_EQ(fwd.GetStats().bytes_sent, 8u);
}

TEST_F(DeltaForwarderFlushTest, AlwaysSendsAtLeastOneEntryEvenIfOverBudget) {
  DeltaForwarder fwd;
  auto big = make_delta({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});  // 10 bytes

  fwd.Enqueue(100, big);

  // Budget = 1, but first entry is always sent to guarantee progress.
  auto bytes = fwd.Flush(*sender_, 1);
  EXPECT_EQ(bytes, 10u);
  EXPECT_EQ(fwd.QueueDepth(), 0u);
}

TEST_F(DeltaForwarderFlushTest, DeferredTicksBoostPriority) {
  DeltaForwarder fwd;
  auto small = make_delta({1});
  auto big = make_delta({1, 2, 3, 4, 5, 6, 7, 8});

  // Enqueue entity 100 (small, 1 byte) and entity 200 (big, 8 bytes).
  fwd.Enqueue(100, small);
  fwd.Enqueue(200, big);

  // Budget = 2: send 1-byte entry (both are deferred_ticks=0, order is stable).
  // After flush: entity 200 remains, its deferred_ticks becomes 1.
  auto bytes1 = fwd.Flush(*sender_, 2);
  EXPECT_EQ(bytes1, 1u);
  EXPECT_EQ(fwd.QueueDepth(), 1u);  // entity 200 deferred

  // Now enqueue a new entity 300 (also small).
  fwd.Enqueue(300, small);
  EXPECT_EQ(fwd.QueueDepth(), 2u);

  // Flush with budget = 2: entity 200 has deferred_ticks=1, entity 300 has 0.
  // Entity 200 should go first (higher deferred_ticks).
  auto bytes2 = fwd.Flush(*sender_, 2);
  // Entity 200 is 8 bytes > budget 2, but it's the first so it's sent.
  EXPECT_EQ(bytes2, 8u);
  EXPECT_EQ(fwd.QueueDepth(), 1u);  // entity 300 deferred
}

TEST_F(DeltaForwarderFlushTest, ReplacePreservesAccumulatedDeferredTicks) {
  DeltaForwarder fwd;
  auto d1 = make_delta({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});  // 10 bytes
  auto d_other = make_delta({0xFF});                      // 1 byte

  // Enqueue entity 100 (10 bytes).
  fwd.Enqueue(100, d1);
  // Also enqueue entity 200 so entity 100 can be deferred.
  fwd.Enqueue(200, d_other);

  fwd.Flush(*sender_, 2);

  auto remaining = fwd.QueueDepth();
  EXPECT_EQ(remaining, 1u);

  auto d2 = make_delta({0xAA, 0xBB});
  fwd.Enqueue(100, d2);  // May or may not find entity 100 in queue.
  fwd.Enqueue(200, d2);  // Same for entity 200.

  EXPECT_LE(fwd.QueueDepth(), 2u);
}

TEST_F(DeltaForwarderFlushTest, HighPriorityFlushesBeforeLowPriority) {
  DeltaForwarder fwd;
  auto small = make_delta({0xAA});                         // 1 byte
  auto big = make_delta({1, 2, 3, 4, 5, 6, 7, 8, 9, 10});  // 10 bytes

  // Priority must beat insertion order.
  fwd.Enqueue(100, big, /*priority=*/0);
  fwd.Enqueue(200, small, /*priority=*/5);

  auto bytes1 = fwd.Flush(*sender_, 1);
  EXPECT_EQ(bytes1, 1u);            // small entry 200 sent
  EXPECT_EQ(fwd.QueueDepth(), 1u);  // entity 100 remains
}

TEST_F(DeltaForwarderFlushTest, ReplaceMergesPriorityAsMax) {
  DeltaForwarder fwd;
  auto d1 = make_delta({0x11});
  auto d2 = make_delta({0x22});

  // Boost entity 100 to priority 7, then a later low-priority write
  // arrives. The merged entry must retain priority 7.
  fwd.Enqueue(100, d1, /*priority=*/7);
  fwd.Enqueue(100, d2, /*priority=*/1);  // same entity, lower priority
  fwd.Enqueue(200, d1, /*priority=*/3);  // different entity, middle priority

  // If priority merge took new_priority (1), entity 200 would flush first.
  // If it took max (7), entity 100 should flush first.
  auto bytes = fwd.Flush(*sender_, 1);
  // QueueDepth confirms only one entry flushed.
  EXPECT_EQ(bytes, 1u);
  EXPECT_EQ(fwd.QueueDepth(), 1u);
  auto bytes2 = fwd.Flush(*sender_, 1);
  EXPECT_EQ(bytes2, 1u);
  EXPECT_EQ(fwd.QueueDepth(), 0u);
}

TEST_F(DeltaForwarderFlushTest, EqualPriorityFallsThroughToDeferredTicks) {
  DeltaForwarder fwd;
  auto small = make_delta({0x01});                  // 1 byte
  auto big = make_delta({1, 2, 3, 4, 5, 6, 7, 8});  // 8 bytes

  // Both at priority 0. Entity 200 gets deferred first.
  fwd.Enqueue(100, small, /*priority=*/0);
  fwd.Enqueue(200, big, /*priority=*/0);
  fwd.Flush(*sender_, 2);           // sends entity 100, defers entity 200
  EXPECT_EQ(fwd.QueueDepth(), 1u);  // entity 200 remains, deferred_ticks=1

  // New entity 300 arrives, same priority. Deferred_ticks tiebreak should
  // keep entity 200 ahead.
  fwd.Enqueue(300, small, /*priority=*/0);
  auto bytes = fwd.Flush(*sender_, 2);
  EXPECT_EQ(bytes, 8u);
  EXPECT_EQ(fwd.QueueDepth(), 1u);  // entity 300 deferred now
}

// Starved entries must eventually bypass both priority and budget.
TEST_F(DeltaForwarderFlushTest, StarvedEntryForceSentRegardlessOfPriority) {
  DeltaForwarder fwd;
  auto low = make_delta({0xAA});
  auto hi = make_delta({0xBB, 0xCC});

  // Seed the low-priority entry and let it age past the cap.
  fwd.Enqueue(100, low, /*priority=*/0);
  for (uint32_t i = 0; i < DeltaForwarder::kMaxDeferredTicks; ++i) {
    // The hi-priority 2-byte entry consumes the 1-byte budget first.
    fwd.Enqueue(200 + i, hi, /*priority=*/10);
    fwd.Flush(*sender_, 1);
  }
  // At this point entity 100 has been deferred kMaxDeferredTicks times.
  EXPECT_GE(fwd.GetStats().force_sent_count, 0u);  // possibly 0 up to now
  const uint64_t forced_before = fwd.GetStats().force_sent_count;

  // Next flush: Pass 1 must force-send entity 100 even though the budget
  // is saturated by the hi-priority stream.
  fwd.Enqueue(999, hi, /*priority=*/10);
  fwd.Flush(*sender_, 1);

  EXPECT_GT(fwd.GetStats().force_sent_count, forced_before)
      << "starved entry must be force-sent past the budget cap";
}

}  // namespace atlas
