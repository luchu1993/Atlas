#include <gtest/gtest.h>

#include <limits>
#include <span>
#include <vector>

#include "dbappmgr/dbappmgr.h"
#include "network/channel.h"
#include "network/network_interface.h"
#include "serialization/binary_stream.h"

using namespace atlas;

namespace {

class DBAppMgrHarness {
 public:
  DBAppMgrHarness() : network(dispatcher), mgr(dispatcher, network) {}

  EventDispatcher dispatcher{"dbappmgr_test"};
  NetworkInterface network;
  DBAppMgr mgr;
};

auto Register(DBAppMgr& mgr, uint16_t port, uint32_t known_app_id = 0) -> Address {
  dbappmgr::RegisterDbApp msg;
  msg.internal_addr = Address(0x7F000001u, port);
  msg.known_app_id = known_app_id;
  mgr.OnRegisterDbApp(msg.internal_addr, nullptr, msg);
  return msg.internal_addr;
}

class RecordingChannel final : public Channel {
 public:
  RecordingChannel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote)
      : Channel(dispatcher, table, remote) {}

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }

  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<std::size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }

  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& {
    return sends_;
  }

 private:
  std::vector<std::vector<std::byte>> sends_;
};

auto ShardTableUpdates(const RecordingChannel& ch) -> std::vector<dbappmgr::ShardTableUpdate> {
  std::vector<dbappmgr::ShardTableUpdate> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != dbappmgr::ShardTableUpdate::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader msg_reader(*payload);
    auto msg = dbappmgr::ShardTableUpdate::Deserialize(msg_reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

}  // namespace

TEST(DBAppMgr, RegisterAssignsShardTableAndStableIds) {
  DBAppMgrHarness h;
  const Address first = Register(h.mgr, 24001);

  ASSERT_EQ(h.mgr.DbApps().size(), 1u);
  ASSERT_EQ(h.mgr.ShardTable().size(), 1u);
  EXPECT_EQ(h.mgr.DbApps().at(first).app_id, 1u);
  EXPECT_EQ(h.mgr.ShardTable()[0].low_dbid, 1);
  EXPECT_EQ(h.mgr.ShardTable()[0].high_dbid, std::numeric_limits<DatabaseID>::max());
  EXPECT_EQ(h.mgr.ShardTable()[0].dbapp_id, 1u);
  EXPECT_EQ(h.mgr.ShardTableVersion(), 1u);
}

TEST(DBAppMgr, RegisterPreservesKnownAppIdForRecovery) {
  DBAppMgrHarness h;
  const Address recovered = Register(h.mgr, 24007, 7);
  const Address next = Register(h.mgr, 24008);

  EXPECT_EQ(h.mgr.DbApps().at(recovered).app_id, 7u);
  EXPECT_EQ(h.mgr.DbApps().at(next).app_id, 8u);
}

TEST(DBAppMgr, RegisterSplitsLargestRange) {
  DBAppMgrHarness h;
  Register(h.mgr, 24001);
  Register(h.mgr, 24002);

  ASSERT_EQ(h.mgr.ShardTable().size(), 2u);
  EXPECT_EQ(h.mgr.ShardTable()[0].low_dbid, 1);
  EXPECT_EQ(h.mgr.ShardTable()[0].dbapp_id, 1u);
  EXPECT_EQ(h.mgr.ShardTable()[1].low_dbid, h.mgr.ShardTable()[0].high_dbid);
  EXPECT_EQ(h.mgr.ShardTable()[1].dbapp_id, 2u);

  const DatabaseID right_dbid = h.mgr.ShardTable()[1].low_dbid;
  const auto right = h.mgr.FindShard(right_dbid);
  ASSERT_TRUE(right.has_value());
  EXPECT_EQ(right->dbapp_id, 2u);
}

TEST(DBAppMgr, InformLoadClampsAndRejectsWrongSource) {
  DBAppMgrHarness h;
  const Address addr = Register(h.mgr, 24001);

  dbappmgr::InformLoad load;
  load.dbapp_id = 1;
  load.load = 1.5f;
  load.entity_count = 10;
  load.pending_checkout_count = 2;
  load.write_queue_depth = 3;
  h.mgr.OnInformLoad(addr, nullptr, load);

  const auto& info = h.mgr.DbApps().at(addr);
  EXPECT_EQ(info.load, 1.0f);
  EXPECT_EQ(info.entity_count, 10u);
  EXPECT_EQ(info.pending_checkout_count, 2u);

  load.load = 0.1f;
  h.mgr.OnInformLoad(Address(0x7F000001u, 25000), nullptr, load);
  EXPECT_EQ(h.mgr.DbApps().at(addr).load, 1.0f);
}

TEST(DBAppMgr, DeathReassignsAndMergesShardsToLeastLoadedSurvivor) {
  DBAppMgrHarness h;
  const Address first = Register(h.mgr, 24001);
  const Address second = Register(h.mgr, 24002);
  const Address third = Register(h.mgr, 24003);

  dbappmgr::InformLoad load;
  load.dbapp_id = 1;
  load.load = 0.2f;
  h.mgr.OnInformLoad(first, nullptr, load);
  load.dbapp_id = 3;
  load.load = 0.8f;
  h.mgr.OnInformLoad(third, nullptr, load);

  h.mgr.OnDbAppDeath(second, 1);

  ASSERT_EQ(h.mgr.DbApps().size(), 2u);
  ASSERT_EQ(h.mgr.DbApps().count(second), 0u);
  const auto first_shard = h.mgr.FindShard(h.mgr.ShardTable().front().low_dbid);
  ASSERT_TRUE(first_shard.has_value());
  EXPECT_NE(first_shard->dbapp_id, 2u);
  for (const auto& shard : h.mgr.ShardTable()) {
    EXPECT_NE(shard.dbapp_id, 2u);
  }
}

TEST(DBAppMgr, WatchersExposeShardTable) {
  DBAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  Register(h.mgr, 24001);

  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("dbappmgr/dbapp_count").value_or(""), "1");
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("dbappmgr/shards/count").value_or(""), "1");
  EXPECT_NE(h.mgr.GetWatcherRegistry().Get("dbappmgr/shards/table").value_or("").find("24001"),
            std::string::npos);
}

TEST(DBAppMgr, GetShardTableSubscribesForUpdates) {
  DBAppMgrHarness h;
  const Address client_addr(0x7F000001u, 25001);
  RecordingChannel client(h.dispatcher, h.network.InterfaceTable(), client_addr);

  dbappmgr::GetShardTable request;
  request.request_id = 100;
  h.mgr.OnGetShardTable(client_addr, &client, request);

  Register(h.mgr, 24001);

  const auto updates = ShardTableUpdates(client);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates[0].version, h.mgr.ShardTableVersion());
  ASSERT_EQ(updates[0].entries.size(), 1u);
  EXPECT_EQ(updates[0].entries[0].dbapp_addr.Port(), 24001u);
}

TEST(DBAppMgr, RegisteredDbAppsReceiveShardTableUpdates) {
  DBAppMgrHarness h;
  const Address first_addr(0x7F000001u, 24001);
  const Address second_addr(0x7F000001u, 24002);
  RecordingChannel first_ch(h.dispatcher, h.network.InterfaceTable(), first_addr);

  dbappmgr::RegisterDbApp first;
  first.internal_addr = first_addr;
  h.mgr.OnRegisterDbApp(first_addr, &first_ch, first);
  Register(h.mgr, second_addr.Port());

  const auto updates = ShardTableUpdates(first_ch);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates[0].version, h.mgr.ShardTableVersion());
  ASSERT_EQ(updates[0].entries.size(), 2u);
  EXPECT_EQ(updates[0].entries[1].dbapp_addr.Port(), second_addr.Port());
}
