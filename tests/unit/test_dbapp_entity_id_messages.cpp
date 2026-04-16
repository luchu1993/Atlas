#include <gtest/gtest.h>

#include "dbapp_messages.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::dbapp;

namespace {

template <typename Msg>
auto round_trip(const Msg& msg) -> std::optional<Msg> {
  BinaryWriter w;
  msg.Serialize(w);
  auto buf = w.Detach();

  BinaryReader r(buf);
  auto result = Msg::Deserialize(r);
  if (!result) return std::nullopt;
  return std::move(*result);
}

}  // namespace

// ============================================================================
// GetEntityIds
// ============================================================================

TEST(DbappEntityIdMessages, GetEntityIdsRoundTrip) {
  GetEntityIds msg;
  msg.count = 256;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->count, 256u);
}

TEST(DbappEntityIdMessages, GetEntityIdsZeroCount) {
  GetEntityIds msg;
  msg.count = 0;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->count, 0u);
}

// ============================================================================
// GetEntityIdsAck
// ============================================================================

TEST(DbappEntityIdMessages, GetEntityIdsAckRoundTrip) {
  GetEntityIdsAck msg;
  msg.start = 1000;
  msg.end = 1255;
  msg.count = 256;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->start, 1000u);
  EXPECT_EQ(out->end, 1255u);
  EXPECT_EQ(out->count, 256u);
}

TEST(DbappEntityIdMessages, GetEntityIdsAckSingleId) {
  GetEntityIdsAck msg;
  msg.start = 42;
  msg.end = 42;
  msg.count = 1;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->start, 42u);
  EXPECT_EQ(out->end, 42u);
  EXPECT_EQ(out->count, 1u);
}

// ============================================================================
// PutEntityIds
// ============================================================================

TEST(DbappEntityIdMessages, PutEntityIdsRoundTrip) {
  PutEntityIds msg;
  msg.start = 500;
  msg.end = 750;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->start, 500u);
  EXPECT_EQ(out->end, 750u);
}

// ============================================================================
// PutEntityIdsAck
// ============================================================================

TEST(DbappEntityIdMessages, PutEntityIdsAckSuccess) {
  PutEntityIdsAck msg;
  msg.success = true;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->success);
}

TEST(DbappEntityIdMessages, PutEntityIdsAckFailure) {
  PutEntityIdsAck msg;
  msg.success = false;

  auto out = round_trip(msg);
  ASSERT_TRUE(out.has_value());
  EXPECT_FALSE(out->success);
}

// ============================================================================
// Descriptor IDs — verify message IDs match the registry
// ============================================================================

TEST(DbappEntityIdMessages, DescriptorIds) {
  EXPECT_EQ(GetEntityIds::Descriptor().id, 4020);
  EXPECT_EQ(GetEntityIdsAck::Descriptor().id, 4021);
  EXPECT_EQ(PutEntityIds::Descriptor().id, 4022);
  EXPECT_EQ(PutEntityIdsAck::Descriptor().id, 4023);
}
