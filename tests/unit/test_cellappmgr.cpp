#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp_messages.h"
#include "cellappmgr/bsp_tree.h"
#include "cellappmgr/cellappmgr.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "network/address.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "platform/filesystem.h"
#include "platform/io_poller.h"
#include "serialization/binary_stream.h"

namespace atlas {
namespace {

class TestCellAppMgr final : public CellAppMgr {
 public:
  using CellAppMgr::CellAppMgr;

  void RegisterWatchersForTest() {
    RegisterWatchers();
    (void)GetWatcherRegistry().Set("cellappmgr/lb/load_report_stale_ms", "3000");
    (void)GetWatcherRegistry().Set("cellappmgr/lb/retire/drain_watchdog_ms", "30000");
  }

  void OnTickCompleteForTest() { OnTickComplete(); }
};

// Thin harness: real EventDispatcher + NetworkInterface for ServerApp ctors.
struct CellAppMgrHarness {
  EventDispatcher dispatcher{"cellappmgr-test"};
  NetworkInterface network{dispatcher};
  TestCellAppMgr mgr{dispatcher, network};
  // Pin the startup quiescence window to zero so OnCreateSpaceRequest fires
  // ExecuteCreateSpace synchronously instead of deferring 2s for stragglers.
  CellAppMgrHarness() { mgr.SetStartupQuiescenceWindowForTest(Duration::zero()); }
};

auto MakePeerAddr(uint16_t port) -> Address {
  return Address(0x7F000001u, port);
}

void ExpectBoundsEq(const CellBounds& actual, const CellBounds& expected) {
  EXPECT_FLOAT_EQ(actual.min_x, expected.min_x);
  EXPECT_FLOAT_EQ(actual.min_z, expected.min_z);
  EXPECT_FLOAT_EQ(actual.max_x, expected.max_x);
  EXPECT_FLOAT_EQ(actual.max_z, expected.max_z);
}

// Minimal Channel subclass that captures the last frame written to
// DoSend. Lets tests observe outbound traffic without a live network.
class RecordingChannel final : public Channel {
 public:
  RecordingChannel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote)
      : Channel(dispatcher, table, remote) {}

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }

  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }

  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }

 private:
  std::vector<std::vector<std::byte>> sends_;
};

// Pull the first baseapp::CellAppDeath payload out of a RecordingChannel
// capture. Channel SendMessage frames carry a msg_id prefix + length
// header followed by the serialised struct; the InterfaceTable we ship
// to outbound sends uses the standard descriptor so the decoder here
// matches the test_watcher_forwarder helper shape.
auto FirstCellAppDeath(const RecordingChannel& ch) -> baseapp::CellAppDeath {
  // OnCellAppDeath now also fans out a SpaceBspGeometry (rehome triggers
  // BroadcastGeometry); scan by msg_id rather than taking front().
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != baseapp::CellAppDeath::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader msg_reader(*payload);
    auto msg = baseapp::CellAppDeath::Deserialize(msg_reader);
    if (msg.HasValue()) return *msg;
  }
  ADD_FAILURE() << "No CellAppDeath message in the channel's sends";
  return {};
}

auto SpaceBspGeometryMessages(const RecordingChannel& ch)
    -> std::vector<baseapp::SpaceBspGeometry> {
  std::vector<baseapp::SpaceBspGeometry> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != baseapp::SpaceBspGeometry::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader msg_reader(*payload);
    auto msg = baseapp::SpaceBspGeometry::Deserialize(msg_reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

auto RemoveCellMessages(const RecordingChannel& ch)
    -> std::vector<cellappmgr::RemoveCellFromSpace> {
  std::vector<cellappmgr::RemoveCellFromSpace> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellappmgr::RemoveCellFromSpace::Descriptor().id) continue;
    auto msg = cellappmgr::RemoveCellFromSpace::Deserialize(reader);
    if (msg.HasValue()) out.push_back(*msg);
  }
  return out;
}

auto AddCellMessages(const RecordingChannel& ch) -> std::vector<cellappmgr::AddCellToSpace> {
  std::vector<cellappmgr::AddCellToSpace> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellappmgr::AddCellToSpace::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader msg_reader(*payload);
    auto msg = cellappmgr::AddCellToSpace::Deserialize(msg_reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

auto UpdateGeometryMessages(const RecordingChannel& ch)
    -> std::vector<cellappmgr::UpdateGeometry> {
  std::vector<cellappmgr::UpdateGeometry> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellappmgr::UpdateGeometry::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader msg_reader(*payload);
    auto msg = cellappmgr::UpdateGeometry::Deserialize(msg_reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

auto RegisterCellAppAcks(const RecordingChannel& ch)
    -> std::vector<cellappmgr::RegisterCellAppAck> {
  std::vector<cellappmgr::RegisterCellAppAck> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellappmgr::RegisterCellAppAck::Descriptor().id) continue;
    auto msg = cellappmgr::RegisterCellAppAck::Deserialize(reader);
    if (msg.HasValue()) out.push_back(*msg);
  }
  return out;
}


// ============================================================================
// Register / Ack
// ============================================================================

TEST(CellAppMgr, Register_AssignsAppIdAndStoresPeer) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp msg;
  msg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(msg.internal_addr, /*ch=*/nullptr, msg);

  ASSERT_EQ(h.mgr.CellApps().size(), 1u);
  auto it = h.mgr.CellApps().find(msg.internal_addr);
  ASSERT_NE(it, h.mgr.CellApps().end());
  EXPECT_EQ(it->second.app_id, 1u);  // first CellApp → id 1 (0 reserved)
  EXPECT_EQ(it->second.internal_addr.Port(), 30001u);
}

TEST(CellAppMgr, Register_DuplicateIsRejected) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp msg;
  msg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(msg.internal_addr, nullptr, msg);
  h.mgr.OnRegisterCellApp(msg.internal_addr, nullptr, msg);
  // Duplicate ignored — still one entry, still app_id 1.
  EXPECT_EQ(h.mgr.CellApps().size(), 1u);
}

TEST(CellAppMgr, Register_MonotonicAppIds) {
  CellAppMgrHarness h;
  for (uint16_t i = 0; i < 5; ++i) {
    cellappmgr::RegisterCellApp msg;
    msg.internal_addr = MakePeerAddr(30001u + i);
    h.mgr.OnRegisterCellApp(msg.internal_addr, nullptr, msg);
  }
  ASSERT_EQ(h.mgr.CellApps().size(), 5u);
  // Collect app_ids and verify distinct, contiguous, starting at 1.
  std::vector<uint32_t> ids;
  for (const auto& [_, info] : h.mgr.CellApps()) ids.push_back(info.app_id);
  std::sort(ids.begin(), ids.end());
  for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], i + 1);
}

// ============================================================================
// InformCellLoad
// ============================================================================

TEST(CellAppMgr, InformCellLoad_UpdatesPeerAndLeafLoad) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  // Create a Space hosted on that peer so a BSP leaf exists.
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  csr.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  ASSERT_EQ(h.mgr.Spaces().count(42), 1u);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.73f;
  load.entity_count = 42;
  load.cells.push_back({1, 42, 0.f, 0.f, 1});
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  const auto& info = h.mgr.CellApps().at(reg.internal_addr);
  EXPECT_NEAR(info.load, 0.73f, 1e-5f);
  EXPECT_EQ(info.entity_count, 42u);

  // Leaf picked up the load too.
  const auto& partition = h.mgr.Spaces().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_NEAR(leaves[0]->load, 0.73f, 1e-5f);
}

TEST(CellAppMgr, InformCellLoad_WatchersExposeLbState) {
  CellAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();

  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.73f;
  load.entity_count = 42;
  load.cells.push_back({1, 42, 12.5f, -7.25f, 1});
  load.cells.back().script_tick_us = 25000;
  load.cells.back().native_tick_us = 3000;
  load.cells.back().x_buckets = {0, 0, 10, 20, 12, 0, 0, 0};
  load.cells.back().x_load_buckets = {0, 0, 100, 200, 120, 0, 0, 0};
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  const auto cellapps = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/cellapps");
  ASSERT_TRUE(cellapps.has_value());
  EXPECT_NE(cellapps->find("cellapps=1"), std::string::npos);
  EXPECT_NE(cellapps->find("app=1"), std::string::npos);
  EXPECT_NE(cellapps->find("load=0.730"), std::string::npos);
  EXPECT_NE(cellapps->find("entities=42"), std::string::npos);
  EXPECT_NE(cellapps->find("load_age_ms="), std::string::npos);
  EXPECT_NE(cellapps->find("load_stale=0"), std::string::npos);
  const auto stale_count =
      h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/load_report_stale_count");
  ASSERT_TRUE(stale_count.has_value());
  EXPECT_EQ(*stale_count, "0");

  const auto spaces = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/spaces");
  ASSERT_TRUE(spaces.has_value());
  EXPECT_NE(spaces->find("spaces=1"), std::string::npos);
  EXPECT_NE(spaces->find("space=42"), std::string::npos);
  EXPECT_NE(spaces->find("version=1"), std::string::npos);
  EXPECT_NE(spaces->find("pending_ack=0"), std::string::npos);
  EXPECT_NE(spaces->find("cell=1"), std::string::npos);
  EXPECT_NE(spaces->find("tick=0.730"), std::string::npos);
  EXPECT_NE(spaces->find("script_us=25000"), std::string::npos);
  EXPECT_NE(spaces->find("native_us=3000"), std::string::npos);
  EXPECT_NE(spaces->find("witnesses=0"), std::string::npos);
  EXPECT_NE(spaces->find("median=(12.5,-7.2)"), std::string::npos);
  EXPECT_NE(spaces->find("xb=[0,0,10,20,12,0,0,0]"), std::string::npos);
  EXPECT_NE(spaces->find("xlb=[0,0,100,200,120,0,0,0]"), std::string::npos);

  EXPECT_TRUE(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/weights/aoi_peer").has_value());
  EXPECT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/weights/aoi_peer", "0.001"));
}

TEST(CellAppMgr, InformCellLoad_WatchersExposeStaleLoadReports) {
  CellAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();

  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/load_report_stale_ms", "0"));

  const auto stale_count =
      h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/load_report_stale_count");
  ASSERT_TRUE(stale_count.has_value());
  EXPECT_EQ(*stale_count, "1");

  const auto cellapps = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/cellapps");
  ASSERT_TRUE(cellapps.has_value());
  EXPECT_NE(cellapps->find("load_age_ms="), std::string::npos);
  EXPECT_NE(cellapps->find("load_stale=1"), std::string::npos);

  EXPECT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/load_report_stale_ms", "3000"));
}

TEST(CellAppMgr, InformCellLoad_StaleGeometryVersionIsIgnored) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  cellappmgr::InformCellLoad stale;
  stale.app_id = 1;
  stale.load = 0.9f;
  stale.entity_count = 10;
  stale.cells.push_back({1, 10, 4.f, 5.f, 999});
  h.mgr.OnInformCellLoad(Address{}, nullptr, stale);

  const auto& partition = h.mgr.Spaces().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->entity_count, 0u);
  EXPECT_FLOAT_EQ(leaves[0]->load, 0.f);
}

TEST(CellAppMgr, InformCellLoad_WeightedMetricsRaiseLeafLoad) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.05f;
  load.entity_count = 1;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = 1;
  rep.entity_count = 1;
  rep.geometry_version = 1;
  rep.tick_load = 0.05f;
  rep.script_tick_us = 50000;
  rep.native_tick_us = 7000;
  rep.witness_count = 10;
  rep.aoi_peer_count = 100;
  rep.aoi_reliable_bytes = 1024ull * 1024ull;
  rep.aoi_unreliable_bytes = 1024ull * 1024ull;
  rep.backup_bytes = 1024ull * 1024ull;
  load.cells.push_back(rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  const auto& partition = h.mgr.Spaces().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_NEAR(leaves[0]->tick_load, 0.05f, 1e-5f);
  EXPECT_EQ(leaves[0]->script_tick_us, 50000u);
  EXPECT_EQ(leaves[0]->native_tick_us, 7000u);
  EXPECT_EQ(leaves[0]->witness_count, 10u);
  EXPECT_EQ(leaves[0]->aoi_peer_count, 100u);
  EXPECT_GT(leaves[0]->load, 1.9f);
}

TEST(CellAppMgr, GrowSpacesForNewCellApp_UsesBucketHistogramForSplit) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.9f;
  load.entity_count = 100;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = 1;
  rep.entity_count = 100;
  rep.median_x = -750.f;
  rep.geometry_version = 1;
  rep.x_buckets = {0, 50, 0, 0, 0, 50, 0, 0};
  load.cells.push_back(rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  const auto& partition = h.mgr.Spaces().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  const auto* left = partition.bsp.FindCell(-1.f, 0.f);
  const auto* right = partition.bsp.FindCell(1.f, 0.f);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->cell_id, 1u);
  EXPECT_EQ(right->cellapp_addr, reg_b.internal_addr);
  EXPECT_FLOAT_EQ(left->bounds.max_x, 0.f);
  EXPECT_FLOAT_EQ(right->bounds.min_x, 0.f);
}

TEST(CellAppMgr, GrowSpacesForNewCellApp_PrefersLoadBucketsForSplit) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.9f;
  load.entity_count = 80;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = 1;
  rep.entity_count = 80;
  rep.median_x = 0.f;
  rep.geometry_version = 1;
  rep.x_buckets = {10, 10, 10, 10, 10, 10, 10, 10};
  rep.x_load_buckets = {30, 30, 30, 10, 0, 0, 0, 0};
  load.cells.push_back(rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  const auto& partition = h.mgr.Spaces().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  const auto* left = partition.bsp.FindCell(-750.f, 0.f);
  const auto* right = partition.bsp.FindCell(-250.f, 0.f);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->cell_id, 1u);
  EXPECT_EQ(right->cellapp_addr, reg_b.internal_addr);
  EXPECT_FLOAT_EQ(left->bounds.max_x, -500.f);
  EXPECT_FLOAT_EQ(right->bounds.min_x, -500.f);
}

TEST(CellAppMgr, InformCellLoad_ClampsNegativeAndOverflow) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = -0.5f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  EXPECT_FLOAT_EQ(h.mgr.CellApps().at(reg.internal_addr).load, 0.f);

  load.load = 5.0f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  EXPECT_FLOAT_EQ(h.mgr.CellApps().at(reg.internal_addr).load, 1.f);
}

TEST(CellAppMgr, InformCellLoad_UnknownAppIdIsIgnored) {
  CellAppMgrHarness h;
  cellappmgr::InformCellLoad load;
  load.app_id = 99;
  load.load = 0.5f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);  // must not crash
  EXPECT_TRUE(h.mgr.CellApps().empty());
}

TEST(CellAppMgr, InformCellLoadRequiresRegisteredSender) {
  CellAppMgrHarness h;
  InterfaceTable table;
  RecordingChannel ch_a(h.dispatcher, table, MakePeerAddr(30001));
  RecordingChannel ch_b(h.dispatcher, table, MakePeerAddr(30002));

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, &ch_a, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, &ch_b, reg_b);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  const auto app_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  cellappmgr::InformCellLoad load;
  load.app_id = app_a;
  load.load = 0.8f;
  load.entity_count = 10;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = partition.bsp.PrimaryCellId();
  rep.entity_count = 10;
  rep.geometry_version = partition.geometry_version;
  rep.tick_load = 0.8f;
  load.cells.push_back(rep);

  h.mgr.OnInformCellLoad(reg_b.internal_addr, &ch_b, load);
  const auto* leaf = partition.bsp.FindCellById(rep.cell_id);
  ASSERT_NE(leaf, nullptr);
  EXPECT_EQ(h.mgr.CellApps().at(reg_a.internal_addr).load, 0.f);
  EXPECT_EQ(leaf->load, 0.f);

  h.mgr.OnInformCellLoad(reg_a.internal_addr, &ch_a, load);
  leaf = partition.bsp.FindCellById(rep.cell_id);
  ASSERT_NE(leaf, nullptr);
  EXPECT_FLOAT_EQ(h.mgr.CellApps().at(reg_a.internal_addr).load, 0.8f);
  EXPECT_FLOAT_EQ(leaf->load, 0.8f);
}

TEST(CellAppMgr, InformCellLoad_PerCellReportFromNonOwnerIsIgnored) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  CellInfo right;
  right.cell_id = 2;
  right.cellapp_addr = reg_b.internal_addr;
  ASSERT_TRUE(partition.bsp.Split(1, BSPAxis::kX, 0.f, right).HasValue());

  const uint32_t app_id_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  cellappmgr::InformCellLoad bad;
  bad.app_id = app_id_a;
  bad.load = 0.9f;
  bad.entity_count = 100;
  bad.cells.push_back({2, 100u, 25.f, 0.f, 1});
  h.mgr.OnInformCellLoad(Address{}, nullptr, bad);

  const auto* ignored = partition.bsp.FindCellById(2);
  ASSERT_NE(ignored, nullptr);
  EXPECT_FLOAT_EQ(ignored->load, 0.f);
  EXPECT_EQ(ignored->entity_count, 0u);

  const uint32_t app_id_b = h.mgr.CellApps().at(reg_b.internal_addr).app_id;
  cellappmgr::InformCellLoad good;
  good.app_id = app_id_b;
  good.load = 0.8f;
  good.entity_count = 80;
  good.cells.push_back({2, 80u, 25.f, 0.f, 1});
  h.mgr.OnInformCellLoad(Address{}, nullptr, good);

  const auto* updated = partition.bsp.FindCellById(2);
  ASSERT_NE(updated, nullptr);
  EXPECT_FLOAT_EQ(updated->load, 0.8f);
  EXPECT_EQ(updated->entity_count, 80u);
}

// ============================================================================
// CreateSpaceRequest — host selection + BSP seeding
// ============================================================================

TEST(CellAppMgr, CreateSpace_NoCellAppsAvailable_Drops) {
  CellAppMgrHarness h;
  cellappmgr::CreateSpaceRequest msg;
  msg.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, msg);
  EXPECT_TRUE(h.mgr.Spaces().empty());
}

TEST(CellAppMgr, CreateSpace_PicksLeastLoadedCellApp) {
  CellAppMgrHarness h;
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}, uint16_t{30003}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }
  // Make peer 30002 (app_id 2) lightest.
  cellappmgr::InformCellLoad l1;
  l1.app_id = 1;
  l1.load = 0.8f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, l1);
  cellappmgr::InformCellLoad l2;
  l2.app_id = 2;
  l2.load = 0.1f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, l2);
  cellappmgr::InformCellLoad l3;
  l3.app_id = 3;
  l3.load = 0.5f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, l3);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 77;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  ASSERT_EQ(h.mgr.Spaces().count(77), 1u);

  const auto& partition = h.mgr.Spaces().at(77);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, MakePeerAddr(30002));
}

TEST(CellAppMgr, CreateSpace_SkipsStaleLoadReportHosts) {
  CellAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();

  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/load_report_stale_ms", "0"));

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 77;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  EXPECT_TRUE(h.mgr.Spaces().empty());
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_space_creates"),
            std::optional<std::string>("1"));
  auto pending = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_space_create_status");
  ASSERT_TRUE(pending.has_value());
  EXPECT_NE(pending->find("pending=1 assignable=0"), std::string::npos);
  EXPECT_NE(pending->find("space=77"), std::string::npos);
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/load_report_stale_ms", "3000"));

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.25f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  ASSERT_EQ(h.mgr.Spaces().count(77), 1u);
  const auto leaves = h.mgr.Spaces().at(77).bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, reg.internal_addr);
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_space_creates"),
            std::optional<std::string>("0"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_space_create_status"),
            std::optional<std::string>("pending=0"));
}

TEST(CellAppMgr, CreateSpace_TieBrokenByLowestAppId) {
  CellAppMgrHarness h;
  // Two peers registered, both idle.
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto leaves = h.mgr.Spaces().at(1).bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, MakePeerAddr(30001));  // app_id 1 wins.
}

TEST(CellAppMgr, CreateSpace_DuplicateIsDropped) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  EXPECT_EQ(h.mgr.Spaces().size(), 1u);
}

TEST(CellAppMgr, CreateSpace_MultiCellBootstrapHandlesSixteenCells) {
  CellAppMgrHarness h;
  constexpr uint16_t kCount = 16;
  for (uint16_t i = 0; i < kCount; ++i) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(static_cast<uint16_t>(30001 + i));
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 16;
  csr.initial_cell_count = kCount;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  const auto& partition = h.mgr.Spaces().at(16);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), kCount);

  std::set<uint16_t> ports;
  for (const auto* leaf : leaves) {
    ports.insert(leaf->cellapp_addr.Port());
    EXPECT_LT(leaf->bounds.min_x, leaf->bounds.max_x);
    EXPECT_LT(leaf->bounds.min_z, leaf->bounds.max_z);
  }
  EXPECT_EQ(ports.size(), kCount);
}

TEST(CellAppMgr, CellAppDeath_RemovesPeer) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  EXPECT_EQ(h.mgr.CellApps().size(), 1u);

  h.mgr.OnCellAppDeath(reg.internal_addr, 1);
  EXPECT_TRUE(h.mgr.CellApps().empty());
}

TEST(CellAppMgr, CellAppDeath_RemovesPendingGeometryForDeadTarget) {
  CellAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 122;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  const auto primary = h.mgr.Spaces().at(122).bsp.PrimaryCellId();
  cellappmgr::InformCellLoad load;
  load.app_id = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  load.load = 0.9f;
  load.entity_count = 10;
  load.cells.push_back({primary, 10u, 0.f, 0.f, h.mgr.Spaces().at(122).geometry_version});
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  auto pending = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  ASSERT_EQ(*pending, "1");

  h.mgr.OnCellAppDeath(reg_b.internal_addr, 1);
  pending = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(*pending, "0");
}

TEST(CellAppMgr, CellAppDeath_UnknownAddrSilent) {
  CellAppMgrHarness h;
  h.mgr.OnCellAppDeath(MakePeerAddr(9999), 1);  // must not crash
  EXPECT_TRUE(h.mgr.CellApps().empty());
}

// A death with surviving peers rehomes every orphaned leaf onto a
// survivor so BSP routing stays correct.
TEST(CellAppMgr, CellAppDeath_UnsplitsOrphanedLeafIntoSibling) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 7;
  csr.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  const auto primary_id_before = h.mgr.Spaces().at(7).bsp.PrimaryCellId();
  ASSERT_EQ(h.mgr.Spaces().at(7).bsp.Leaves()[0]->cellapp_addr, reg_a.internal_addr);

  CellInfo right;
  right.cell_id = 999;
  right.cellapp_addr = reg_b.internal_addr;
  ASSERT_TRUE(
      h.mgr.SpacesForTest().at(7).bsp.Split(primary_id_before, BSPAxis::kX, 0.f, right).HasValue());

  h.mgr.OnCellAppDeath(reg_a.internal_addr, 1);

  const auto& after = h.mgr.Spaces().at(7);
  ASSERT_EQ(after.bsp.Leaves().size(), 1u);
  EXPECT_EQ(after.bsp.PrimaryCellId(), 999u);
  EXPECT_EQ(after.bsp.Leaves()[0]->cellapp_addr, reg_b.internal_addr);
  EXPECT_EQ(after.bsp.FindCellById(primary_id_before), nullptr);
  const auto decision = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/last_decision");
  ASSERT_TRUE(decision.has_value());
  EXPECT_NE(decision->find("action=unsplit"), std::string::npos);
  EXPECT_NE(decision->find("reason=cellapp-death"), std::string::npos);
  EXPECT_NE(decision->find("detail=sibling_absorb=1,broadcast_pending=1"), std::string::npos);
  EXPECT_NE(decision->find("before_leaves=2,after_leaves=1"), std::string::npos);
  EXPECT_NE(decision->find("leaf_changes=2"), std::string::npos);
  EXPECT_NE(decision->find(std::format("{}{{app:", primary_id_before)), std::string::npos);
  EXPECT_NE(decision->find("->missing"), std::string::npos);
}

TEST(CellAppMgr, CellAppDeath_RehomeNotificationUsesAbsorbingSibling) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  cellappmgr::RegisterCellApp reg_c;
  reg_c.internal_addr = MakePeerAddr(30003);
  h.mgr.OnRegisterCellApp(reg_c.internal_addr, nullptr, reg_c);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 8;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(8);
  CellInfo right;
  right.cell_id = 2;
  right.cellapp_addr = reg_b.internal_addr;
  ASSERT_TRUE(partition.bsp.Split(1, BSPAxis::kX, 0.f, right).HasValue());
  CellInfo top_right;
  top_right.cell_id = 3;
  top_right.cellapp_addr = reg_c.internal_addr;
  ASSERT_TRUE(partition.bsp.Split(2, BSPAxis::kZ, 0.f, top_right).HasValue());

  InterfaceTable base_table;
  RecordingChannel base_ch(h.dispatcher, base_table, MakePeerAddr(20000));
  h.mgr.BaseAppChannelsForTest()[MakePeerAddr(20000)] = &base_ch;

  h.mgr.OnCellAppDeath(reg_c.internal_addr, 1);

  const auto* absorbing = h.mgr.Spaces().at(8).bsp.FindCell(500.f, 500.f);
  ASSERT_NE(absorbing, nullptr);
  EXPECT_EQ(absorbing->cellapp_addr, reg_b.internal_addr);

  const auto notify = FirstCellAppDeath(base_ch);
  ASSERT_EQ(notify.rehomes.size(), 1u);
  EXPECT_EQ(notify.rehomes[0].first, 8u);
  EXPECT_EQ(notify.rehomes[0].second, reg_b.internal_addr);
  ASSERT_EQ(notify.rehome_cells.size(), 2u);
  const auto* rehome_cell = baseapp::FindRehomeCellForPosition(notify, 8, {500.f, 0.f, 500.f});
  ASSERT_NE(rehome_cell, nullptr);
  EXPECT_EQ(rehome_cell->host_addr, reg_b.internal_addr);
}

TEST(CellAppMgr, CellAppDeath_RehomesLeavesToSurvivor) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  h.mgr.RegisterWatchersForTest();

  // Space is created on whichever mgr picks — that's the to-be-killed
  // peer in this test (app_id 1 wins the tie on lowest app_id at zero
  // load). Assert the initial host then kill it.
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 7;
  csr.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  ASSERT_EQ(h.mgr.Spaces().count(7), 1u);
  const auto& partition_before = h.mgr.Spaces().at(7);
  ASSERT_EQ(partition_before.bsp.Leaves().size(), 1u);
  const Address initial_host = partition_before.bsp.Leaves()[0]->cellapp_addr;
  ASSERT_EQ(initial_host, reg_a.internal_addr)
      << "CreateSpace should pick the lowest app_id under tied load";

  // Kill the initial host. The surviving peer (app_b) must end up as
  // the leaf's cellapp_addr.
  h.mgr.OnCellAppDeath(reg_a.internal_addr, 1);

  const auto& partition_after = h.mgr.Spaces().at(7);
  ASSERT_EQ(partition_after.bsp.Leaves().size(), 1u);
  EXPECT_EQ(partition_after.bsp.Leaves()[0]->cellapp_addr, reg_b.internal_addr)
      << "dead leaf must rehome to the surviving peer";
  EXPECT_EQ(h.mgr.CellApps().size(), 1u);
  EXPECT_EQ(h.mgr.CellApps().begin()->second.internal_addr, reg_b.internal_addr);
  const auto decision = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/last_decision");
  ASSERT_TRUE(decision.has_value());
  EXPECT_NE(decision->find("action=rehome"), std::string::npos);
  EXPECT_NE(decision->find("reason=cellapp-death"), std::string::npos);
  EXPECT_NE(decision->find("detail=add_cell=1,broadcast_pending=1"), std::string::npos);
  EXPECT_NE(decision->find("before_leaves=1,after_leaves=1"), std::string::npos);
  EXPECT_NE(decision->find("leaf_changes=1"), std::string::npos);
  EXPECT_NE(decision->find("app:1->2"), std::string::npos);
}

// Multi-leaf Space: several leaves on the dead app all get rehomed to
// survivor(s) in a single pass. Verifies the loop doesn't bail after the
// first reassignment.
TEST(CellAppMgr, CellAppDeath_RehomesAllMatchingLeaves) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  // Two single-cell Spaces both hosted on app_a (tie break on app_id).
  cellappmgr::CreateSpaceRequest csr1;
  csr1.space_id = 10;
  csr1.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr1);
  cellappmgr::CreateSpaceRequest csr2;
  csr2.space_id = 11;
  csr2.request_id = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr2);
  ASSERT_EQ(h.mgr.Spaces().at(10).bsp.Leaves()[0]->cellapp_addr, reg_a.internal_addr);
  ASSERT_EQ(h.mgr.Spaces().at(11).bsp.Leaves()[0]->cellapp_addr, reg_a.internal_addr);

  h.mgr.OnCellAppDeath(reg_a.internal_addr, 1);

  EXPECT_EQ(h.mgr.Spaces().at(10).bsp.Leaves()[0]->cellapp_addr, reg_b.internal_addr);
  EXPECT_EQ(h.mgr.Spaces().at(11).bsp.Leaves()[0]->cellapp_addr, reg_b.internal_addr);
}

// Death with no survivors is log-only; leaves remain pointing at the
// dead addr so a subsequent CellApp join can optionally reclaim them
// (not implemented). Without this guard the code would deref a nullptr
// alt host.
TEST(CellAppMgr, CellAppDeath_LastPeerLeavesSpacesOrphaned) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 100;
  csr.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  h.mgr.OnCellAppDeath(reg_a.internal_addr, 1);

  EXPECT_TRUE(h.mgr.CellApps().empty());
  // Space retained; leaf still claims the dead addr (defensive — a
  // reviving CellApp is the intended recovery path).
  ASSERT_EQ(h.mgr.Spaces().at(100).bsp.Leaves().size(), 1u);
  EXPECT_EQ(h.mgr.Spaces().at(100).bsp.Leaves()[0]->cellapp_addr, reg_a.internal_addr);
}

// ============================================================================
// TickLoadBalance — safety sanity
// ============================================================================

TEST(CellAppMgr, TickLoadBalance_EmptyIsSafe) {
  CellAppMgrHarness h;
  h.mgr.TickLoadBalance();  // no spaces, no crash
}

TEST(CellAppMgr, TickLoadBalance_SingleSpace_NoCrash) {
  // Single leaf (no internal nodes) → Balance is a no-op, but the
  // tick must still complete cleanly and broadcast the (unchanged)
  // geometry. We can't observe the broadcast here (channel is null);
  // the value is just crash-safety.
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  h.mgr.TickLoadBalance();
  EXPECT_EQ(h.mgr.Spaces().size(), 1u);
}

// Asymmetric load across two BSP leaves must shrink the hot side and
// trigger a broadcast of the updated geometry. Walks the full path:
//   Register two CellApps → CreateSpace → manually seed a two-cell BSP
//   (the mgr's production API doesn't yet expose a "split an existing
//   space" call; tests reach through SpacesForTest) → push asymmetric
//   InformCellLoad → call TickLoadBalance repeatedly → assert the BSP
//   split moved AND the broadcast-cache blob is different from the
//   pre-balance baseline.
TEST(CellAppMgr, TickLoadBalance_AsymmetricLoad_MovesSplitAndRebroadcasts) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  // Seed a two-cell split: cell 1 stays on A, new cell 2 lands on B.
  // Production will expose a dedicated "split cell" admin RPC; the test
  // reaches past that gap because the rebalance logic it exercises is
  // independent of how the split got there.
  auto& partition = h.mgr.SpacesForTest().at(1);
  const uint32_t app_id_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  const uint32_t app_id_b = h.mgr.CellApps().at(reg_b.internal_addr).app_id;
  CellInfo new_cell{/*cell_id=*/2, reg_b.internal_addr, CellBounds{}, /*load=*/0.f,
                    /*entity_count=*/0};
  auto split = partition.bsp.Split(/*existing_cell_id=*/1, BSPAxis::kX, /*position=*/0.f, new_cell);
  ASSERT_TRUE(split.HasValue());

  // Baseline snapshot of the split line — this is what Balance should
  // shift when the load is heavier on the left side.
  const auto* leaf_left = partition.bsp.FindCell(-100.f, 0.f);
  const auto* leaf_right = partition.bsp.FindCell(+100.f, 0.f);
  ASSERT_NE(leaf_left, nullptr);
  ASSERT_NE(leaf_right, nullptr);
  EXPECT_EQ(leaf_left->cell_id, 1u);
  EXPECT_EQ(leaf_right->cell_id, 2u);

  // Baseline broadcast blob (CreateSpace's initial fan-out was with
  // the single-cell tree; our Split above didn't re-broadcast, so the
  // cache is out of date vs the current in-memory tree. That's fine —
  // the assertion below is "blob changes across the balance calls",
  // which it does either way).
  const auto baseline_blob = partition.last_broadcast_blob;

  // Feed asymmetric load: A (left half) heavy at 0.9, B (right half)
  // light at 0.1. Reported via wire message so the whole OnInformCellLoad
  // path runs, including cellapps_[] load update + per-leaf load mirror.
  cellappmgr::InformCellLoad load_a;
  load_a.app_id = app_id_a;
  load_a.load = 0.9f;
  load_a.entity_count = 900;
  load_a.cells.push_back({1, 900u, -100.f, 0.f, partition.geometry_version});
  h.mgr.OnInformCellLoad(Address{}, nullptr, load_a);
  cellappmgr::InformCellLoad load_b;
  load_b.app_id = app_id_b;
  load_b.load = 0.1f;
  load_b.entity_count = 100;
  load_b.cells.push_back({2, 100u, 100.f, 0.f, partition.geometry_version});
  h.mgr.OnInformCellLoad(Address{}, nullptr, load_b);

  for (int i = 0; i < 5; ++i) h.mgr.TickLoadBalance();

  // Serialize fresh and compare. The cached blob must now differ from
  // the pre-balance state — or, equivalently, equal a freshly serialised
  // current tree.
  BinaryWriter w;
  partition.bsp.Serialize(w);
  const auto current_bytes_vec = w.Detach();
  const std::vector<std::byte> current_bytes(current_bytes_vec.begin(), current_bytes_vec.end());
  EXPECT_EQ(partition.last_broadcast_blob, current_bytes)
      << "cache should equal the freshly-serialised tree after rebalance";
  EXPECT_NE(partition.last_broadcast_blob, baseline_blob)
      << "rebalance should have changed the broadcast blob from its seeded baseline";

  const auto* left_after = partition.bsp.FindCellById(1);
  const auto* right_after = partition.bsp.FindCellById(2);
  ASSERT_NE(left_after, nullptr);
  ASSERT_NE(right_after, nullptr);
  EXPECT_LT(right_after->bounds.min_x, 0.f)
      << "with left heavier, split should move left and shrink the hot side";
  EXPECT_FLOAT_EQ(left_after->bounds.max_x, right_after->bounds.min_x);
}

TEST(CellAppMgr, TickLoadBalance_UsesBucketHistogramForContinuousSplit) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(1);
  const uint32_t app_id_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  const uint32_t app_id_b = h.mgr.CellApps().at(reg_b.internal_addr).app_id;
  CellInfo new_cell{/*cell_id=*/2, reg_b.internal_addr, CellBounds{}, /*load=*/0.f,
                    /*entity_count=*/0};
  auto split = partition.bsp.Split(/*existing_cell_id=*/1, BSPAxis::kX, /*position=*/0.f,
                                   new_cell);
  ASSERT_TRUE(split.HasValue());

  cellappmgr::InformCellLoad load_a;
  load_a.app_id = app_id_a;
  load_a.load = 0.8f;
  load_a.entity_count = 80;
  cellappmgr::InformCellLoad::CellReport rep_a;
  rep_a.cell_id = 1;
  rep_a.entity_count = 80;
  rep_a.geometry_version = partition.geometry_version;
  rep_a.tick_load = 0.8f;
  rep_a.x_buckets = {10, 10, 10, 10, 10, 10, 10, 10};
  load_a.cells.push_back(rep_a);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load_a);

  cellappmgr::InformCellLoad load_b;
  load_b.app_id = app_id_b;
  load_b.load = 0.2f;
  load_b.entity_count = 20;
  cellappmgr::InformCellLoad::CellReport rep_b;
  rep_b.cell_id = 2;
  rep_b.entity_count = 20;
  rep_b.geometry_version = partition.geometry_version;
  rep_b.tick_load = 0.2f;
  rep_b.x_buckets = {10, 10, 10, 10, 10, 10, 10, 10};
  load_b.cells.push_back(rep_b);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load_b);

  h.mgr.TickLoadBalance();

  const auto* left_after = partition.bsp.FindCellById(1);
  const auto* right_after = partition.bsp.FindCellById(2);
  ASSERT_NE(left_after, nullptr);
  ASSERT_NE(right_after, nullptr);
  EXPECT_NEAR(right_after->bounds.min_x, -375.f, 1e-5f);
  EXPECT_FLOAT_EQ(left_after->bounds.max_x, right_after->bounds.min_x);
}

TEST(CellAppMgr, TickLoadBalance_AutoSplitsSustainedHotLeafToIdleHost) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  cellappmgr::InformCellLoad load;
  load.app_id = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  load.load = 0.95f;
  load.entity_count = 100;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = partition.bsp.PrimaryCellId();
  rep.entity_count = 100;
  rep.geometry_version = partition.geometry_version;
  rep.tick_load = 0.95f;
  rep.x_buckets = {0, 50, 0, 0, 0, 50, 0, 0};
  load.cells.push_back(rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  h.mgr.TickLoadBalance();
  h.mgr.TickLoadBalance();
  EXPECT_EQ(partition.bsp.Leaves().size(), 1u);

  h.mgr.TickLoadBalance();

  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  const auto* left = partition.bsp.FindCell(-1.f, 0.f);
  const auto* right = partition.bsp.FindCell(1.f, 0.f);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->cell_id, rep.cell_id);
  EXPECT_EQ(right->cellapp_addr, reg_b.internal_addr);
  EXPECT_FLOAT_EQ(left->bounds.max_x, 0.f);
  EXPECT_FLOAT_EQ(right->bounds.min_x, 0.f);
  const auto decision = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/last_decision");
  ASSERT_TRUE(decision.has_value());
  EXPECT_NE(decision->find("action=split"), std::string::npos);
  EXPECT_NE(decision->find("reason=auto-split"), std::string::npos);
  EXPECT_NE(decision->find("space=42"), std::string::npos);
  EXPECT_NE(decision->find(std::format("cell={}", rep.cell_id)), std::string::npos);
  EXPECT_NE(decision->find("target_app=2"), std::string::npos);
  EXPECT_NE(decision->find("before_leaves=1,after_leaves=2"), std::string::npos);
  EXPECT_NE(decision->find("leaf_changes=2"), std::string::npos);
  EXPECT_NE(decision->find("app:missing->2"), std::string::npos);
  EXPECT_NE(decision->find("bounds:missing->(0.0/-1000.0/1000.0/1000.0)"),
            std::string::npos);
  const auto decision_count = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/decision_count");
  ASSERT_TRUE(decision_count.has_value());
  EXPECT_EQ(*decision_count, "1");
}

TEST(CellAppMgr, AddCellToSpaceAckRequiresTargetAndSuccess) {
  CellAppMgrHarness h;
  InterfaceTable table;
  RecordingChannel ch_a(h.dispatcher, table, MakePeerAddr(30001));
  RecordingChannel ch_b(h.dispatcher, table, MakePeerAddr(30002));

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, &ch_a, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, &ch_b, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  cellappmgr::InformCellLoad load;
  load.app_id = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  load.load = 0.95f;
  load.entity_count = 100;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = partition.bsp.PrimaryCellId();
  rep.entity_count = 100;
  rep.geometry_version = partition.geometry_version;
  rep.tick_load = 0.95f;
  load.cells.push_back(rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  h.mgr.TickLoadBalance();
  h.mgr.TickLoadBalance();
  h.mgr.TickLoadBalance();

  const auto adds_b = AddCellMessages(ch_b);
  ASSERT_FALSE(adds_b.empty());
  const auto new_cell_id = adds_b.back().cell_id;
  const auto updates_a_before = UpdateGeometryMessages(ch_a).size();
  const auto updates_b_before = UpdateGeometryMessages(ch_b).size();
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts"),
            std::optional<std::string>("1"));

  cellappmgr::AddCellToSpaceAck ack;
  ack.space_id = 42;
  ack.cell_id = new_cell_id;
  h.mgr.OnAddCellToSpaceAck(reg_a.internal_addr, &ch_a, ack);
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts"),
            std::optional<std::string>("1"));
  EXPECT_EQ(UpdateGeometryMessages(ch_a).size(), updates_a_before);
  EXPECT_EQ(UpdateGeometryMessages(ch_b).size(), updates_b_before);

  ack.success = false;
  h.mgr.OnAddCellToSpaceAck(reg_b.internal_addr, &ch_b, ack);
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts"),
            std::optional<std::string>("1"));
  EXPECT_EQ(UpdateGeometryMessages(ch_a).size(), updates_a_before);
  EXPECT_EQ(UpdateGeometryMessages(ch_b).size(), updates_b_before);

  ack.success = true;
  h.mgr.OnAddCellToSpaceAck(reg_b.internal_addr, &ch_b, ack);
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts"),
            std::optional<std::string>("0"));
  EXPECT_GT(UpdateGeometryMessages(ch_a).size(), updates_a_before);
  EXPECT_GT(UpdateGeometryMessages(ch_b).size(), updates_b_before);
}

TEST(CellAppMgr, TickLoadBalance_PendingGeometryFreezesBspUntilAck) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  cellappmgr::InformCellLoad load;
  load.app_id = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  load.load = 0.95f;
  load.entity_count = 100;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = partition.bsp.PrimaryCellId();
  rep.entity_count = 100;
  rep.geometry_version = partition.geometry_version;
  rep.tick_load = 0.95f;
  load.cells.push_back(rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  h.mgr.TickLoadBalance();
  h.mgr.TickLoadBalance();
  h.mgr.TickLoadBalance();

  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  const auto* first_before = partition.bsp.FindCellById(leaves[0]->cell_id);
  const auto* second_before = partition.bsp.FindCellById(leaves[1]->cell_id);
  ASSERT_NE(first_before, nullptr);
  ASSERT_NE(second_before, nullptr);
  const auto first_bounds = first_before->bounds;
  const auto second_bounds = second_before->bounds;
  const auto version = partition.geometry_version;

  for (int i = 0; i < 5; ++i) h.mgr.TickLoadBalance();

  const auto* first_after = partition.bsp.FindCellById(first_before->cell_id);
  const auto* second_after = partition.bsp.FindCellById(second_before->cell_id);
  ASSERT_NE(first_after, nullptr);
  ASSERT_NE(second_after, nullptr);
  ExpectBoundsEq(first_after->bounds, first_bounds);
  ExpectBoundsEq(second_after->bounds, second_bounds);
  EXPECT_EQ(partition.geometry_version, version);
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts"),
            std::optional<std::string>("1"));
}

TEST(CellAppMgr, TickLoadBalance_AutoMergesSustainedIdleSiblingLeaf) {
  CellAppMgrHarness h;
  InterfaceTable table;
  RecordingChannel ch_b(h.dispatcher, table, MakePeerAddr(30002));

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, &ch_b, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  csr.initial_cell_count = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  const auto b_it = std::find_if(leaves.begin(), leaves.end(), [&](const CellInfo* leaf) {
    return leaf->cellapp_addr == reg_b.internal_addr;
  });
  ASSERT_NE(b_it, leaves.end());
  const auto b_cell_id = (*b_it)->cell_id;

  for (const auto* leaf : leaves) {
    const auto app_id = h.mgr.CellApps().at(leaf->cellapp_addr).app_id;
    cellappmgr::InformCellLoad load;
    load.app_id = app_id;
    load.load = 0.05f;
    load.entity_count = 0;
    cellappmgr::InformCellLoad::CellReport rep;
    rep.cell_id = leaf->cell_id;
    rep.entity_count = 0;
    rep.geometry_version = partition.geometry_version;
    rep.tick_load = 0.05f;
    load.cells.push_back(rep);
    h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  }

  h.mgr.TickLoadBalance();
  h.mgr.TickLoadBalance();
  EXPECT_EQ(partition.bsp.Leaves().size(), 2u);

  h.mgr.TickLoadBalance();

  leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cell_id, partition.bsp.PrimaryCellId());
  const auto removes = RemoveCellMessages(ch_b);
  ASSERT_FALSE(removes.empty());
  EXPECT_EQ(removes.back().space_id, 42u);
  EXPECT_EQ(removes.back().cell_id, b_cell_id);
  const auto decision = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/last_decision");
  ASSERT_TRUE(decision.has_value());
  EXPECT_NE(decision->find("action=merge"), std::string::npos);
  EXPECT_NE(decision->find("reason=auto-merge"), std::string::npos);
  EXPECT_NE(decision->find(std::format("cell={}", b_cell_id)), std::string::npos);
  EXPECT_NE(decision->find("detail=removed_empty=1"), std::string::npos);
  EXPECT_NE(decision->find("before_leaves=2,after_leaves=1"), std::string::npos);
  EXPECT_NE(decision->find("leaf_changes=2"), std::string::npos);
  EXPECT_NE(decision->find(std::format("{}{{app:", b_cell_id)), std::string::npos);
  EXPECT_NE(decision->find("->missing"), std::string::npos);
}

TEST(CellAppMgr, RetireWatcherSkipsNewSpacePlacement) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  h.mgr.RegisterWatchersForTest();

  const auto app_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  const auto app_b = h.mgr.CellApps().at(reg_b.internal_addr).app_id;
  cellappmgr::InformCellLoad load_a;
  load_a.app_id = app_a;
  load_a.load = 0.9f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load_a);
  cellappmgr::InformCellLoad load_b;
  load_b.app_id = app_b;
  load_b.load = 0.0f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load_b);

  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/retire/app_id",
                                             std::to_string(app_b)));
  const auto retire_id = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/app_id");
  ASSERT_TRUE(retire_id.has_value());
  EXPECT_EQ(*retire_id, std::to_string(app_b));
  const auto retire_count = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/count");
  ASSERT_TRUE(retire_count.has_value());
  EXPECT_EQ(*retire_count, "1");

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto leaves = h.mgr.SpacesForTest().at(42).bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, reg_a.internal_addr);

  const auto summary = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/cellapps");
  ASSERT_TRUE(summary.has_value());
  EXPECT_NE(summary->find("retiring=1"), std::string::npos);
}

TEST(CellAppMgr, TickLoadBalance_RetireRemovesEmptyNonPrimaryLeaf) {
  CellAppMgrHarness h;
  InterfaceTable table;
  RecordingChannel ch_b(h.dispatcher, table, MakePeerAddr(30002));

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, &ch_b, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  csr.initial_cell_count = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  const auto b_it = std::find_if(leaves.begin(), leaves.end(), [&](const CellInfo* leaf) {
    return leaf->cellapp_addr == reg_b.internal_addr;
  });
  ASSERT_NE(b_it, leaves.end());
  const auto b_cell_id = (*b_it)->cell_id;

  for (const auto* leaf : leaves) {
    const bool on_b = leaf->cellapp_addr == reg_b.internal_addr;
    const auto app_id = h.mgr.CellApps().at(leaf->cellapp_addr).app_id;
    cellappmgr::InformCellLoad load;
    load.app_id = app_id;
    load.load = on_b ? 0.6f : 0.9f;
    load.entity_count = on_b ? 0u : 10u;
    cellappmgr::InformCellLoad::CellReport rep;
    rep.cell_id = leaf->cell_id;
    rep.entity_count = load.entity_count;
    rep.geometry_version = partition.geometry_version;
    rep.tick_load = load.load;
    load.cells.push_back(rep);
    h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  }

  const auto app_b = h.mgr.CellApps().at(reg_b.internal_addr).app_id;
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/retire/app_id",
                                             std::to_string(app_b)));

  h.mgr.TickLoadBalance();

  leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, reg_a.internal_addr);
  EXPECT_EQ(partition.bsp.FindCellById(b_cell_id), nullptr);
  const auto removes = RemoveCellMessages(ch_b);
  ASSERT_FALSE(removes.empty());
  EXPECT_EQ(removes.back().space_id, 42u);
  EXPECT_EQ(removes.back().cell_id, b_cell_id);
  const auto decision = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/last_decision");
  ASSERT_TRUE(decision.has_value());
  EXPECT_NE(decision->find("action=remove"), std::string::npos);
  EXPECT_NE(decision->find("reason=retire-empty"), std::string::npos);
  EXPECT_NE(decision->find(std::format("source_app={}", app_b)), std::string::npos);
  EXPECT_NE(decision->find("detail=empty=1"), std::string::npos);
  EXPECT_NE(decision->find("before_leaves=2,after_leaves=1"), std::string::npos);
  EXPECT_NE(decision->find("leaf_changes=2"), std::string::npos);
  EXPECT_NE(decision->find(std::format("{}{{app:", b_cell_id)), std::string::npos);
  EXPECT_NE(decision->find("->missing"), std::string::npos);
}

TEST(CellAppMgr, TickLoadBalance_RetireHandsOffNonEmptyNonPrimaryLeaf) {
  CellAppMgrHarness h;
  InterfaceTable table;
  RecordingChannel ch_a(h.dispatcher, table, MakePeerAddr(30001));
  RecordingChannel ch_b(h.dispatcher, table, MakePeerAddr(30002));

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, &ch_a, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, &ch_b, reg_b);
  h.mgr.RegisterWatchersForTest();
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/retire/drain_watchdog_ms", "0"));

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  csr.initial_cell_count = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  const auto b_it = std::find_if(leaves.begin(), leaves.end(), [&](const CellInfo* leaf) {
    return leaf->cellapp_addr == reg_b.internal_addr;
  });
  ASSERT_NE(b_it, leaves.end());
  const auto b_cell_id = (*b_it)->cell_id;
  const auto updates_b_before = UpdateGeometryMessages(ch_b).size();
  const auto adds_a_before = AddCellMessages(ch_a).size();

  for (const auto* leaf : leaves) {
    const bool on_b = leaf->cellapp_addr == reg_b.internal_addr;
    const auto app_id = h.mgr.CellApps().at(leaf->cellapp_addr).app_id;
    cellappmgr::InformCellLoad load;
    load.app_id = app_id;
    load.load = on_b ? 0.7f : 0.3f;
    load.entity_count = on_b ? 5u : 10u;
    cellappmgr::InformCellLoad::CellReport rep;
    rep.cell_id = leaf->cell_id;
    rep.entity_count = load.entity_count;
    rep.geometry_version = partition.geometry_version;
    rep.tick_load = load.load;
    load.cells.push_back(rep);
    h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  }

  const auto app_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  const auto app_b = h.mgr.CellApps().at(reg_b.internal_addr).app_id;
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/retire/app_id",
                                             std::to_string(app_b)));

  h.mgr.TickLoadBalance();

  const auto adds_a_after = AddCellMessages(ch_a);
  ASSERT_GT(adds_a_after.size(), adds_a_before);
  EXPECT_EQ(adds_a_after.back().space_id, 42u);
  EXPECT_EQ(adds_a_after.back().cell_id, b_cell_id);
  EXPECT_FALSE(adds_a_after.back().is_primary);
  const auto* handed_leaf = partition.bsp.FindCellById(b_cell_id);
  ASSERT_NE(handed_leaf, nullptr);
  EXPECT_EQ(handed_leaf->cellapp_addr, reg_a.internal_addr);
  EXPECT_EQ(UpdateGeometryMessages(ch_b).size(), updates_b_before);
  const auto drain_count = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(drain_count.has_value());
  EXPECT_EQ(*drain_count, "1");
  const auto pending_stuck = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/stuck_count");
  ASSERT_TRUE(pending_stuck.has_value());
  EXPECT_EQ(*pending_stuck, "0");
  const auto decision = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/last_decision");
  ASSERT_TRUE(decision.has_value());
  EXPECT_NE(decision->find("action=handoff"), std::string::npos);
  EXPECT_NE(decision->find("reason=retire-drain"), std::string::npos);
  EXPECT_NE(decision->find(std::format("source_app={}", app_b)), std::string::npos);
  EXPECT_NE(decision->find(std::format("target_app={}", app_a)), std::string::npos);
  EXPECT_NE(decision->find("before_leaves=2,after_leaves=2"), std::string::npos);
  EXPECT_NE(decision->find("leaf_changes=1"), std::string::npos);
  EXPECT_NE(decision->find(std::format("{}{{app:{}->{}", b_cell_id, app_b, app_a)),
            std::string::npos);

  cellappmgr::AddCellToSpaceAck ack;
  ack.space_id = 42;
  ack.cell_id = b_cell_id;
  h.mgr.OnAddCellToSpaceAck(reg_a.internal_addr, nullptr, ack);

  EXPECT_GT(UpdateGeometryMessages(ch_b).size(), updates_b_before);
  const auto stuck = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/stuck_count");
  ASSERT_TRUE(stuck.has_value());
  EXPECT_EQ(*stuck, "1");
  const auto stuck_status = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/status");
  ASSERT_TRUE(stuck_status.has_value());
  EXPECT_NE(stuck_status->find(std::format("app={} owned=0 drains=1 pending=0 ready=0 "
                                           "stuck=1",
                                           app_b)),
            std::string::npos);

  cellappmgr::InformCellLoad drained;
  drained.app_id = app_b;
  drained.load = 0.0f;
  drained.entity_count = 0;
  cellappmgr::InformCellLoad::CellReport drained_rep;
  drained_rep.cell_id = b_cell_id;
  drained_rep.entity_count = 0;
  drained_rep.geometry_version = partition.geometry_version;
  drained.cells.push_back(drained_rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, drained);

  const auto removes = RemoveCellMessages(ch_b);
  ASSERT_FALSE(removes.empty());
  EXPECT_EQ(removes.back().space_id, 42u);
  EXPECT_EQ(removes.back().cell_id, b_cell_id);
  const auto finished = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(finished.has_value());
  EXPECT_EQ(*finished, "0");
  const auto stuck_finished = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/stuck_count");
  ASSERT_TRUE(stuck_finished.has_value());
  EXPECT_EQ(*stuck_finished, "0");
  const auto status = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find(std::format("app={} owned=0 drains=0 pending=0 ready=1", app_b)),
            std::string::npos);
  const auto history_size =
      h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/decision_history_size");
  ASSERT_TRUE(history_size.has_value());
  EXPECT_EQ(*history_size, "2");
  const auto history = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/decision_history");
  ASSERT_TRUE(history.has_value());
  EXPECT_NE(history->find("decisions=2"), std::string::npos);
  EXPECT_NE(history->find("action=handoff"), std::string::npos);
  EXPECT_NE(history->find("action=drain-complete"), std::string::npos);
  EXPECT_NE(history->find("before_drains=1,after_drains=0"), std::string::npos);
}

TEST(CellAppMgr, TickLoadBalance_RetireHandsOffPrimaryLeafToInSpaceReplica) {
  CellAppMgrHarness h;
  InterfaceTable table;
  RecordingChannel ch_a(h.dispatcher, table, MakePeerAddr(30001));
  RecordingChannel ch_b(h.dispatcher, table, MakePeerAddr(30002));

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, &ch_a, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, &ch_b, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  csr.initial_cell_count = 2;
  csr.space_master_type = "SpaceMaster";
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  const auto primary_id = partition.bsp.PrimaryCellId();
  const auto* primary = partition.bsp.FindCellById(primary_id);
  ASSERT_NE(primary, nullptr);
  ASSERT_EQ(primary->cellapp_addr, reg_a.internal_addr);
  const auto updates_a_before = UpdateGeometryMessages(ch_a).size();
  const auto adds_b_before = AddCellMessages(ch_b).size();

  for (const auto* leaf : partition.bsp.Leaves()) {
    const bool on_primary = leaf->cell_id == primary_id;
    const auto app_id = h.mgr.CellApps().at(leaf->cellapp_addr).app_id;
    cellappmgr::InformCellLoad load;
    load.app_id = app_id;
    load.load = on_primary ? 0.7f : 0.3f;
    load.entity_count = on_primary ? 3u : 10u;
    cellappmgr::InformCellLoad::CellReport rep;
    rep.cell_id = leaf->cell_id;
    rep.entity_count = load.entity_count;
    rep.geometry_version = partition.geometry_version;
    rep.tick_load = load.load;
    load.cells.push_back(rep);
    h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  }

  const auto app_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/retire/app_id",
                                             std::to_string(app_a)));

  h.mgr.TickLoadBalance();

  const auto adds_b_after = AddCellMessages(ch_b);
  ASSERT_GT(adds_b_after.size(), adds_b_before);
  EXPECT_EQ(adds_b_after.back().space_id, 42u);
  EXPECT_EQ(adds_b_after.back().cell_id, primary_id);
  EXPECT_TRUE(adds_b_after.back().is_primary);
  EXPECT_TRUE(adds_b_after.back().space_master_type.empty());
  EXPECT_EQ(adds_b_after.back().space_data_source_addr, reg_a.internal_addr);
  primary = partition.bsp.FindCellById(primary_id);
  ASSERT_NE(primary, nullptr);
  EXPECT_EQ(primary->cellapp_addr, reg_b.internal_addr);
  EXPECT_EQ(UpdateGeometryMessages(ch_a).size(), updates_a_before);

  cellappmgr::AddCellToSpaceAck ack;
  ack.space_id = 42;
  ack.cell_id = primary_id;
  h.mgr.OnAddCellToSpaceAck(reg_b.internal_addr, nullptr, ack);

  EXPECT_GT(UpdateGeometryMessages(ch_a).size(), updates_a_before);

  cellappmgr::InformCellLoad drained;
  drained.app_id = app_a;
  drained.load = 0.0f;
  drained.entity_count = 0;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = primary_id;
  rep.entity_count = 0;
  rep.geometry_version = partition.geometry_version;
  drained.cells.push_back(rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, drained);

  const auto removes = RemoveCellMessages(ch_a);
  ASSERT_FALSE(removes.empty());
  EXPECT_EQ(removes.back().space_id, 42u);
  EXPECT_EQ(removes.back().cell_id, primary_id);
  const auto status = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find(std::format("app={} owned=0 drains=0 pending=0 ready=1", app_a)),
            std::string::npos);
}

TEST(CellAppMgr, TickLoadBalance_RetireHandsOffSinglePrimaryLeafWithSnapshotSource) {
  CellAppMgrHarness h;
  InterfaceTable table;
  RecordingChannel ch_a(h.dispatcher, table, MakePeerAddr(30001));
  RecordingChannel ch_b(h.dispatcher, table, MakePeerAddr(30002));

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, &ch_a, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, &ch_b, reg_b);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  csr.space_master_type = "SpaceMaster";
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(42);
  const auto primary_id = partition.bsp.PrimaryCellId();
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  ASSERT_EQ(leaves[0]->cellapp_addr, reg_a.internal_addr);
  const auto updates_a_before = UpdateGeometryMessages(ch_a).size();
  const auto adds_b_before = AddCellMessages(ch_b).size();

  cellappmgr::InformCellLoad load;
  load.app_id = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  load.load = 0.7f;
  load.entity_count = 3;
  cellappmgr::InformCellLoad::CellReport rep;
  rep.cell_id = primary_id;
  rep.entity_count = 3;
  rep.geometry_version = partition.geometry_version;
  rep.tick_load = 0.7f;
  load.cells.push_back(rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  const auto app_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/retire/app_id",
                                             std::to_string(app_a)));

  h.mgr.TickLoadBalance();

  const auto adds_b_after = AddCellMessages(ch_b);
  ASSERT_GT(adds_b_after.size(), adds_b_before);
  EXPECT_EQ(adds_b_after.back().space_id, 42u);
  EXPECT_EQ(adds_b_after.back().cell_id, primary_id);
  EXPECT_TRUE(adds_b_after.back().is_primary);
  EXPECT_TRUE(adds_b_after.back().space_master_type.empty());
  EXPECT_EQ(adds_b_after.back().space_data_source_addr, reg_a.internal_addr);
  const auto* primary = partition.bsp.FindCellById(primary_id);
  ASSERT_NE(primary, nullptr);
  EXPECT_EQ(primary->cellapp_addr, reg_b.internal_addr);
  EXPECT_EQ(UpdateGeometryMessages(ch_a).size(), updates_a_before);

  cellappmgr::AddCellToSpaceAck ack;
  ack.space_id = 42;
  ack.cell_id = primary_id;
  h.mgr.OnAddCellToSpaceAck(reg_b.internal_addr, nullptr, ack);

  EXPECT_GT(UpdateGeometryMessages(ch_a).size(), updates_a_before);

  cellappmgr::InformCellLoad drained;
  drained.app_id = app_a;
  drained.load = 0.0f;
  drained.entity_count = 0;
  cellappmgr::InformCellLoad::CellReport drained_rep;
  drained_rep.cell_id = primary_id;
  drained_rep.entity_count = 0;
  drained_rep.geometry_version = partition.geometry_version;
  drained.cells.push_back(drained_rep);
  h.mgr.OnInformCellLoad(Address{}, nullptr, drained);

  const auto removes = RemoveCellMessages(ch_a);
  ASSERT_FALSE(removes.empty());
  EXPECT_EQ(removes.back().space_id, 42u);
  EXPECT_EQ(removes.back().cell_id, primary_id);
}

TEST(CellAppMgr, TickLoadBalance_RetireDrainsMultipleSpacesContinuously) {
  CellAppMgrHarness h;
  InterfaceTable table;
  RecordingChannel ch_a(h.dispatcher, table, MakePeerAddr(30001));
  RecordingChannel ch_b(h.dispatcher, table, MakePeerAddr(30002));

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, &ch_a, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, &ch_b, reg_b);
  h.mgr.RegisterWatchersForTest();

  std::vector<std::pair<SpaceID, cellappmgr::CellID>> primaries;
  for (SpaceID space_id = 41; space_id <= 43; ++space_id) {
    cellappmgr::CreateSpaceRequest csr;
    csr.space_id = space_id;
    csr.initial_cell_count = 2;
    csr.space_master_type = "SpaceMaster";
    h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

    auto& partition = h.mgr.SpacesForTest().at(space_id);
    const auto primary_id = partition.bsp.PrimaryCellId();
    const auto* primary = partition.bsp.FindCellById(primary_id);
    ASSERT_NE(primary, nullptr);
    ASSERT_EQ(primary->cellapp_addr, reg_a.internal_addr);
    primaries.push_back({space_id, primary_id});
  }

  for (const auto& primary : primaries) {
    auto& partition = h.mgr.SpacesForTest().at(primary.first);
    for (const auto* leaf : partition.bsp.Leaves()) {
      const bool on_a = leaf->cellapp_addr == reg_a.internal_addr;
      const auto app_id = h.mgr.CellApps().at(leaf->cellapp_addr).app_id;
      cellappmgr::InformCellLoad load;
      load.app_id = app_id;
      load.load = on_a ? 0.7f : 0.2f;
      load.entity_count = on_a ? 4u : 8u;
      cellappmgr::InformCellLoad::CellReport rep;
      rep.cell_id = leaf->cell_id;
      rep.entity_count = load.entity_count;
      rep.geometry_version = partition.geometry_version;
      rep.tick_load = load.load;
      load.cells.push_back(rep);
      h.mgr.OnInformCellLoad(Address{}, nullptr, load);
    }
  }

  const auto adds_b_before = AddCellMessages(ch_b).size();
  const auto app_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/retire/app_id",
                                             std::to_string(app_a)));

  h.mgr.TickLoadBalance();

  const auto adds_b_after = AddCellMessages(ch_b);
  ASSERT_GE(adds_b_after.size(), adds_b_before + primaries.size());
  std::set<std::pair<SpaceID, cellappmgr::CellID>> handed_off;
  for (std::size_t i = adds_b_before; i < adds_b_after.size(); ++i) {
    if (!adds_b_after[i].is_primary) continue;
    EXPECT_TRUE(adds_b_after[i].space_master_type.empty());
    EXPECT_EQ(adds_b_after[i].space_data_source_addr, reg_a.internal_addr);
    handed_off.insert({adds_b_after[i].space_id, adds_b_after[i].cell_id});
  }
  for (const auto& primary : primaries) {
    EXPECT_TRUE(handed_off.contains(primary));
  }
  auto drain_count = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(drain_count.has_value());
  EXPECT_EQ(*drain_count, "3");
  auto pending = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(*pending, "3");

  for (const auto& [space_id, primary_id] : primaries) {
    cellappmgr::AddCellToSpaceAck ack;
    ack.space_id = space_id;
    ack.cell_id = primary_id;
    h.mgr.OnAddCellToSpaceAck(reg_b.internal_addr, nullptr, ack);
  }
  pending = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(*pending, "0");

  for (const auto& [space_id, primary_id] : primaries) {
    cellappmgr::InformCellLoad drained;
    drained.app_id = app_a;
    drained.load = 0.0f;
    drained.entity_count = 0;
    cellappmgr::InformCellLoad::CellReport rep;
    rep.cell_id = primary_id;
    rep.entity_count = 0;
    rep.geometry_version = h.mgr.SpacesForTest().at(space_id).geometry_version;
    drained.cells.push_back(rep);
    h.mgr.OnInformCellLoad(reg_a.internal_addr, nullptr, drained);
  }

  drain_count = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(drain_count.has_value());
  EXPECT_EQ(*drain_count, "0");
  std::set<std::pair<SpaceID, cellappmgr::CellID>> removed;
  for (const auto& msg : RemoveCellMessages(ch_a)) {
    removed.insert({msg.space_id, msg.cell_id});
  }
  for (const auto& primary : primaries) {
    EXPECT_TRUE(removed.contains(primary));
  }
  const auto status = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find(std::format("app={} owned=0 drains=0 pending=0 ready=1", app_a)),
            std::string::npos);
}

// End-to-end CellApp-death recovery notification to subscribed BaseApps.
// Wire path: mgr's OnCellAppDeath rehomes the BSP leaves it was tracking,
// builds a baseapp::CellAppDeath with the per-Space new-host map, and
// fans it out to every subscribed BaseApp. Observing it needs a
// RecordingChannel injected via BaseAppChannelsForTest — the mgr's
// machined subscription normally fills that map from Birth events.
TEST(CellAppMgr, CellAppDeath_FansOutNotificationToBaseAppSubscribers) {
  CellAppMgrHarness h;
  // Two CellApps: A (will die) and B (will inherit A's leaves).
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  // A space hosted on A by default (OnCreateSpaceRequest picks the
  // least-loaded at tie-break, and app_id 1 wins ties).
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  // Inject a recording baseapp peer so we can observe the fan-out.
  // Production wires this via the mgr's ProcessType::kBaseApp subscribe
  // callback; the test hook bypasses machined.
  InterfaceTable base_table;
  RecordingChannel base_ch(h.dispatcher, base_table, MakePeerAddr(20000));
  h.mgr.BaseAppChannelsForTest()[MakePeerAddr(20000)] = &base_ch;

  // Kill A. Mgr must (a) drop A from cellapps_, (b) rehome cell 1 to B,
  // (c) fan a CellAppDeath wire msg out to every baseapp — with the
  // rehomes list telling BaseApp which space moved where.
  h.mgr.OnCellAppDeath(reg_a.internal_addr, 1);

  // BSP rehome (covered by other tests, but worth a sanity touch here
  // so the death-before-fan-out ordering is visible).
  const auto leaves = h.mgr.Spaces().at(42).bsp.Leaves();
  ASSERT_FALSE(leaves.empty());
  EXPECT_EQ(leaves[0]->cellapp_addr, reg_b.internal_addr);

  // Baseapp peer receives the death notice plus the post-rehome BSP
  // geometry fan-out used by the client-side debug gizmo.
  ASSERT_GE(base_ch.Sends().size(), 1u);
  const auto notify = FirstCellAppDeath(base_ch);
  EXPECT_EQ(notify.dead_addr, reg_a.internal_addr);
  ASSERT_EQ(notify.rehomes.size(), 1u);
  EXPECT_EQ(notify.rehomes[0].first, 42u);
  EXPECT_EQ(notify.rehomes[0].second, reg_b.internal_addr);
  ASSERT_EQ(notify.rehome_cells.size(), 1u);
  EXPECT_EQ(notify.rehome_cells[0].space_id, 42u);
  EXPECT_EQ(notify.rehome_cells[0].host_addr, reg_b.internal_addr);
}

// The broadcast cache short-circuits re-sends when the serialised tree
// hasn't changed. Observable via SpacePartition::last_broadcast_blob:
// first fan-out populates it; subsequent no-op ticks must keep the
// bytes identical (we can't observe the wire directly with null
// channels, so byte-equality is the proxy). A BSP mutation forces a
// fresh cached blob.
// Sequence of MessageIDs seen on a RecordingChannel — used to assert the
// freeze → geometry → unfreeze ordering produced by BroadcastGeometry.
auto MessageIdSequence(const RecordingChannel& ch) -> std::vector<uint16_t> {
  std::vector<uint16_t> ids;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id) continue;
    ids.push_back(static_cast<uint16_t>(*id));
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    (void)reader.ReadBytes(*len);
  }
  return ids;
}

// BroadcastGeometry must atomically freeze every cell, ship the new BSP,
// then unfreeze. Receiver-side ordering is preserved by the reliable
// channel, so an entity can't offload through a stale boundary.
TEST(CellAppMgr, BroadcastGeometry_WrapsWithShouldOffloadFreeze) {
  CellAppMgrHarness h;
  InterfaceTable peer_table;
  RecordingChannel peer_ch(h.dispatcher, peer_table, MakePeerAddr(30001));

  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, &peer_ch, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto seq = MessageIdSequence(peer_ch);
  const auto should_off = static_cast<uint16_t>(msg_id::Id(msg_id::CellAppMgr::kShouldOffload));
  const auto update_geom = static_cast<uint16_t>(msg_id::Id(msg_id::CellAppMgr::kUpdateGeometry));
  auto pos = std::find(seq.begin(), seq.end(), update_geom);
  ASSERT_NE(pos, seq.end()) << "no UpdateGeometry shipped";
  ASSERT_NE(pos, seq.begin()) << "UpdateGeometry must follow a ShouldOffload(false)";
  EXPECT_EQ(*(pos - 1), should_off);
  ASSERT_NE(pos + 1, seq.end()) << "UpdateGeometry must precede a ShouldOffload(true)";
  EXPECT_EQ(*(pos + 1), should_off);
}

TEST(CellAppMgr, BroadcastGeometry_DebugNoticeIncludesLoadAndEntityCount) {
  CellAppMgrHarness h;
  InterfaceTable base_table;
  RecordingChannel base_ch(h.dispatcher, base_table, MakePeerAddr(20000));
  h.mgr.BaseAppChannelsForTest()[MakePeerAddr(20000)] = &base_ch;

  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  const auto initial = SpaceBspGeometryMessages(base_ch);
  ASSERT_EQ(initial.size(), 1u);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.73f;
  load.entity_count = 42;
  load.cells.push_back({1, 42, 12.5f, -7.25f, 1});
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  h.mgr.TickLoadBalance();

  const auto notices = SpaceBspGeometryMessages(base_ch);
  ASSERT_GE(notices.size(), 2u);
  ASSERT_EQ(notices.back().leaves.size(), 1u);
  EXPECT_EQ(notices.back().space_id, 42u);
  EXPECT_EQ(notices.back().leaves[0].cell_id, 1u);
  EXPECT_EQ(notices.back().leaves[0].entity_count, 42u);
  EXPECT_FLOAT_EQ(notices.back().leaves[0].load, 0.73f);
}

TEST(CellAppMgr, BroadcastGeometry_CachesBlob_SkipsUnchangedReSends) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  // CreateSpace already fans out once; cache should now be populated.
  const auto& partition = h.mgr.Spaces().at(1);
  const auto baseline = partition.last_broadcast_blob;
  ASSERT_FALSE(baseline.empty());

  // Idle tick: blob must stay byte-identical. Any re-serialise would
  // produce a structurally equal but new vector — the equality still
  // holds because the cache tracks bytes, so this check also confirms
  // the tree itself didn't drift.
  h.mgr.TickLoadBalance();
  EXPECT_EQ(partition.last_broadcast_blob, baseline);
}

// Builds a RecoverCellAppState report from a source mgr's space partition.
auto MakeRecoverReport(CellAppMgrHarness& src, SpaceID space_id)
    -> cellappmgr::RecoverCellAppState {
  const auto& partition = src.mgr.SpacesForTest().at(space_id);
  BinaryWriter w;
  partition.bsp.Serialize(w);
  const auto blob = w.Detach();
  cellappmgr::RecoverCellAppState report;
  cellappmgr::RecoverCellAppState::SpaceGeometry geom;
  geom.space_id = space_id;
  geom.geometry_version = partition.geometry_version;
  geom.bsp_blob.assign(blob.begin(), blob.end());
  report.spaces.push_back(std::move(geom));
  return report;
}

TEST(CellAppMgr, RegisterAdoptsKnownAppIdOnSnapshotlessRecovery) {
  CellAppMgrHarness recovered;  // fresh mgr, no snapshot
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  reg.known_app_id = 42;  // the id this CellApp owned before the mgr died
  recovered.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  const auto& apps = recovered.mgr.CellApps();
  ASSERT_TRUE(apps.contains(reg.internal_addr));
  EXPECT_EQ(apps.at(reg.internal_addr).app_id, 42u)
      << "snapshot-less recovery must keep the CellApp's known app_id";

  // A subsequent genuinely-new CellApp gets a fresh id past the adopted one.
  cellappmgr::RegisterCellApp fresh;
  fresh.internal_addr = MakePeerAddr(30002);
  recovered.mgr.OnRegisterCellApp(fresh.internal_addr, nullptr, fresh);
  EXPECT_GT(recovered.mgr.CellApps().at(fresh.internal_addr).app_id, 42u);
}

TEST(CellAppMgr, RegisterFallsBackToFreshWhenKnownAppIdCollides) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp a;
  a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(a.internal_addr, nullptr, a);  // gets app_id 1
  const auto a_id = h.mgr.CellApps().at(a.internal_addr).app_id;

  // Second CellApp claims the same known id → must NOT collide; assigned fresh.
  cellappmgr::RegisterCellApp b;
  b.internal_addr = MakePeerAddr(30002);
  b.known_app_id = a_id;
  h.mgr.OnRegisterCellApp(b.internal_addr, nullptr, b);
  EXPECT_NE(h.mgr.CellApps().at(b.internal_addr).app_id, a_id);
}

TEST(CellAppMgr, RecoveryWindowHoldsRestoreGateThenOpens) {
  CellAppMgrHarness h;  // harness zeros the startup window
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 60;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  h.mgr.RegisterWatchersForTest();

  // No snapshot: with the window open the gate must still freeze LB so workers
  // get a chance to report before we balance.
  h.mgr.SetRecoveryDeadlineForTest(Clock::now() + std::chrono::seconds(5));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/recovery_window_active"),
            std::optional<std::string>("true"));
  EXPECT_NE(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/recovery_window_status").value_or("").find(
                "active=1"),
            std::string::npos);

  // Window elapsed -> gate opens.
  h.mgr.SetRecoveryDeadlineForTest(Clock::now() - std::chrono::milliseconds(1));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/recovery_window_active"),
            std::optional<std::string>("false"));
}

TEST(CellAppMgr, RecoverCellAppStateRebuildsSpaceFromWorkerReport) {
  CellAppMgrHarness src;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  src.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 50;
  src.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  const auto version = src.mgr.SpacesForTest().at(50).geometry_version;
  const auto report = MakeRecoverReport(src, 50);

  // Fresh mgr that never had space 50; register the owner so leaf routing
  // resolves, then feed the worker report — the partition is rebuilt without
  // any snapshot.
  CellAppMgrHarness recovered;
  recovered.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  ASSERT_FALSE(recovered.mgr.Spaces().contains(50));
  recovered.mgr.OnRecoverCellAppState(reg.internal_addr, nullptr, report);

  ASSERT_TRUE(recovered.mgr.Spaces().contains(50));
  const auto& rebuilt = recovered.mgr.Spaces().at(50);
  EXPECT_EQ(rebuilt.geometry_version, version);
  const auto leaves = rebuilt.bsp.Leaves();
  ASSERT_FALSE(leaves.empty());
  EXPECT_EQ(leaves[0]->cellapp_addr, reg.internal_addr);
}

TEST(CellAppMgr, RecoverCellAppStateSkipsWhenLeafOwnerUnknown) {
  CellAppMgrHarness src;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  src.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 51;
  src.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  const auto report = MakeRecoverReport(src, 51);

  // Owner never registered on the fresh mgr → cell->app routing can't resolve,
  // so the rebuild is deferred (the report from the last owner to rejoin
  // installs it). Space stays absent.
  CellAppMgrHarness recovered;
  recovered.mgr.OnRecoverCellAppState(reg.internal_addr, nullptr, report);
  EXPECT_FALSE(recovered.mgr.Spaces().contains(51));
}

TEST(CellAppMgr, RecoverCellAppStateIgnoresVersionNotNewerThanExisting) {
  CellAppMgrHarness src;
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  src.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 52;
  src.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);  // owned by 30002
  const auto report = MakeRecoverReport(src, 52);

  // recovered mgr already owns space 52 via 30001 at an equal version; a report
  // that is not strictly newer must not overwrite it.
  CellAppMgrHarness recovered;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  recovered.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  recovered.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  recovered.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);  // owned by 30001
  ASSERT_TRUE(recovered.mgr.Spaces().contains(52));
  const auto existing_version = recovered.mgr.Spaces().at(52).geometry_version;
  ASSERT_EQ(report.spaces.front().geometry_version, existing_version);

  recovered.mgr.OnRecoverCellAppState(reg_b.internal_addr, nullptr, report);

  const auto leaves = recovered.mgr.Spaces().at(52).bsp.Leaves();
  ASSERT_FALSE(leaves.empty());
  EXPECT_EQ(leaves[0]->cellapp_addr, reg_a.internal_addr)
      << "equal-version report overwrote the existing partition";
}

}  // namespace
}  // namespace atlas
