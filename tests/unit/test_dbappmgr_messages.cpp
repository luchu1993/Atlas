#include <gtest/gtest.h>

#include "dbappmgr/dbappmgr_messages.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::dbappmgr;

namespace {

template <typename Msg>
auto RoundTrip(const Msg& msg) -> Msg {
  BinaryWriter writer;
  msg.Serialize(writer);
  BinaryReader reader(writer.Data());
  auto result = Msg::Deserialize(reader);
  EXPECT_TRUE(result.HasValue());
  return result.ValueOr(Msg{});
}

auto MakeShard(DatabaseID low, DatabaseID high, uint32_t id, uint16_t port) -> ShardEntry {
  ShardEntry entry;
  entry.low_dbid = low;
  entry.high_dbid = high;
  entry.dbapp_id = id;
  entry.dbapp_addr = Address(0x7F000001u, port);
  return entry;
}

}  // namespace

TEST(DBAppMgrMessages, RegisterDbAppRoundTrips) {
  RegisterDbApp msg;
  msg.internal_addr = Address(0x7F000001u, 24001);
  msg.known_app_id = 7;
  msg.known_shard_table_version = 42;

  const auto out = RoundTrip(msg);
  EXPECT_EQ(out.internal_addr.Ip(), 0x7F000001u);
  EXPECT_EQ(out.internal_addr.Port(), 24001u);
  EXPECT_EQ(out.known_app_id, 7u);
  EXPECT_EQ(out.known_shard_table_version, 42u);
}

TEST(DBAppMgrMessages, RegisterDbAppAckRoundTripsShardTable) {
  RegisterDbAppAck msg;
  msg.success = true;
  msg.dbapp_id = 2;
  msg.shard_table_version = 5;
  msg.entries.push_back(MakeShard(1, 100, 1, 24001));
  msg.entries.push_back(MakeShard(100, 200, 2, 24002));

  const auto out = RoundTrip(msg);
  ASSERT_TRUE(out.success);
  EXPECT_EQ(out.dbapp_id, 2u);
  EXPECT_EQ(out.shard_table_version, 5u);
  ASSERT_EQ(out.entries.size(), 2u);
  EXPECT_EQ(out.entries[1].low_dbid, 100);
  EXPECT_EQ(out.entries[1].dbapp_addr.Port(), 24002u);
}

TEST(DBAppMgrMessages, InformLoadRoundTrips) {
  InformLoad msg;
  msg.dbapp_id = 3;
  msg.load = 0.75f;
  msg.entity_count = 1000;
  msg.pending_checkout_count = 8;
  msg.write_queue_depth = 11;

  const auto out = RoundTrip(msg);
  EXPECT_EQ(out.dbapp_id, 3u);
  EXPECT_NEAR(out.load, 0.75f, 1e-5f);
  EXPECT_EQ(out.entity_count, 1000u);
  EXPECT_EQ(out.pending_checkout_count, 8u);
  EXPECT_EQ(out.write_queue_depth, 11u);
}

TEST(DBAppMgrMessages, ShardTableResponseRoundTrips) {
  ShardTableResponse msg;
  msg.request_id = 99;
  msg.version = 4;
  msg.entries.push_back(MakeShard(1, 500, 1, 24001));

  const auto out = RoundTrip(msg);
  EXPECT_EQ(out.request_id, 99u);
  EXPECT_EQ(out.version, 4u);
  ASSERT_EQ(out.entries.size(), 1u);
  EXPECT_EQ(out.entries[0].high_dbid, 500);
}

TEST(DBAppMgrMessages, ShardTableUpdateRoundTrips) {
  ShardTableUpdate msg;
  msg.version = 6;
  msg.entries.push_back(MakeShard(1, 500, 1, 24001));
  msg.entries.push_back(MakeShard(500, 900, 2, 24002));

  const auto out = RoundTrip(msg);
  EXPECT_EQ(out.version, 6u);
  ASSERT_EQ(out.entries.size(), 2u);
  EXPECT_EQ(out.entries[1].dbapp_id, 2u);
  EXPECT_EQ(out.entries[1].dbapp_addr.Port(), 24002u);
}

TEST(DBAppMgrMessages, DescriptorIdsUseDBAppMgrRange) {
  EXPECT_EQ(RegisterDbApp::Descriptor().id, 8000);
  EXPECT_EQ(RegisterDbAppAck::Descriptor().id, 8001);
  EXPECT_EQ(InformLoad::Descriptor().id, 8002);
  EXPECT_EQ(GetShardTable::Descriptor().id, 8010);
  EXPECT_EQ(ShardTableResponse::Descriptor().id, 8011);
  EXPECT_EQ(ShardTableUpdate::Descriptor().id, 8012);
  EXPECT_EQ(HealthProbe::Descriptor().id, 8020);
  EXPECT_EQ(HealthProbeAck::Descriptor().id, 8021);
}
