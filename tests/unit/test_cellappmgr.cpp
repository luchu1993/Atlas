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
    (void)GetWatcherRegistry().Set("cellappmgr/ha/reattach_watchdog_ms", "30000");
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

auto CellAppProcessInfo(Address internal_addr, std::string name) -> machined::ProcessInfo {
  machined::ProcessInfo info;
  info.process_type = ProcessType::kCellApp;
  info.name = std::move(name);
  info.internal_addr = internal_addr;
  return info;
}

constexpr uint32_t kSnapshotMagicForTest = 0x314D4143u;
constexpr uint32_t kSnapshotVersionForTest = 4;
constexpr uint64_t kSnapshotChecksumSeedForTest = 14695981039346656037ull;
constexpr uint64_t kSnapshotChecksumPrimeForTest = 1099511628211ull;

auto SnapshotChecksumForTest(std::span<const std::byte> bytes) -> uint64_t {
  uint64_t hash = kSnapshotChecksumSeedForTest;
  for (std::byte byte : bytes) {
    hash ^= std::to_integer<uint8_t>(byte);
    hash *= kSnapshotChecksumPrimeForTest;
  }
  return hash;
}

auto SnapshotPayloadForTest(const std::vector<std::byte>& snapshot) -> std::vector<std::byte> {
  BinaryReader r(std::span<const std::byte>(snapshot.data(), snapshot.size()));
  auto magic = r.Read<uint32_t>();
  auto version = r.Read<uint32_t>();
  auto payload_size = r.Read<uint64_t>();
  auto checksum = r.Read<uint64_t>();
  if (!magic || !version || !payload_size || !checksum ||
      *magic != kSnapshotMagicForTest || *version != kSnapshotVersionForTest) {
    return {};
  }
  auto payload = r.ReadBytes(static_cast<std::size_t>(*payload_size));
  if (!payload || r.Remaining() != 0 || SnapshotChecksumForTest(*payload) != *checksum) return {};
  return std::vector<std::byte>(payload->begin(), payload->end());
}

auto SnapshotWithPayloadForTest(std::span<const std::byte> payload) -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write(kSnapshotMagicForTest);
  w.Write(kSnapshotVersionForTest);
  w.Write(static_cast<uint64_t>(payload.size()));
  w.Write(SnapshotChecksumForTest(payload));
  w.WriteBytes(payload);
  return w.Detach();
}

template <typename T>
auto ReadLittleAtForTest(std::span<const std::byte> bytes, std::size_t offset) -> T {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return endian::FromLittle(value);
}

template <typename T>
void PatchLittleAtForTest(std::vector<std::byte>& bytes, std::size_t offset, T value) {
  const auto le = endian::ToLittle(value);
  std::memcpy(bytes.data() + offset, &le, sizeof(T));
}

auto WatcherInt64ForTest(WatcherRegistry& registry, const char* path)
    -> std::optional<int64_t> {
  auto value = registry.Get(path);
  if (!value) return std::nullopt;
  int64_t parsed = 0;
  const auto* begin = value->data();
  const auto* end = begin + value->size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end) return std::nullopt;
  return parsed;
}

auto CellAppRecordOffsetForTest(std::span<const std::byte> payload, uint32_t index)
    -> std::optional<std::size_t> {
  BinaryReader r(payload);
  r.Skip(sizeof(uint32_t) + sizeof(cellappmgr::CellID) + sizeof(uint64_t) + sizeof(uint32_t));
  auto count = r.ReadPackedInt();
  if (!count || index >= *count) return std::nullopt;
  constexpr std::size_t record_bytes =
      sizeof(uint32_t) + sizeof(uint16_t) + 3 * sizeof(uint32_t) + 2 * sizeof(uint8_t);
  for (uint32_t i = 0; i < *count; ++i) {
    const auto offset = r.Position();
    if (i == index) return offset;
    r.Skip(record_bytes);
  }
  return std::nullopt;
}

class ShutdownSnapshotCellAppMgr final : public CellAppMgr {
 public:
  using CellAppMgr::CellAppMgr;

 protected:
  auto RunLoop() -> bool override {
    SetStartupQuiescenceWindowForTest(Duration::zero());

    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(30101);
    OnRegisterCellApp(reg.internal_addr, nullptr, reg);

    cellappmgr::CreateSpaceRequest csr;
    csr.space_id = 301;
    OnCreateSpaceRequest(Address{}, nullptr, csr);
    return true;
  }
};

class FailingPeriodicSnapshotCellAppMgr final : public CellAppMgr {
 public:
  using CellAppMgr::CellAppMgr;

  [[nodiscard]] auto PeriodicSnapshotFailuresForTest() const -> const std::string& {
    return periodic_snapshot_failures_;
  }

 protected:
  auto RunLoop() -> bool override {
    std::error_code ec;
    std::filesystem::remove_all(Config().snapshot_path, ec);
    if (!fs::CreateDirectories(Config().snapshot_path).HasValue()) return false;

    OnTickComplete();
    OnTickComplete();
    OnTickComplete();

    auto failures = GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_failures");
    periodic_snapshot_failures_ = failures.value_or("missing");
    return true;
  }

 private:
  std::string periodic_snapshot_failures_{"missing"};
};

class DirtySnapshotCellAppMgr final : public CellAppMgr {
 public:
  using CellAppMgr::CellAppMgr;

  [[nodiscard]] auto DirtyBeforeFlushForTest() const -> const std::string& {
    return dirty_before_flush_;
  }
  [[nodiscard]] auto DirtyAfterFlushForTest() const -> const std::string& {
    return dirty_after_flush_;
  }
  [[nodiscard]] auto DirtyReasonAfterFlushForTest() const -> const std::string& {
    return dirty_reason_after_flush_;
  }
  [[nodiscard]] auto SavesAfterFlushForTest() const -> const std::string& {
    return saves_after_flush_;
  }
  [[nodiscard]] auto StatusAfterFlushForTest() const -> const std::string& {
    return status_after_flush_;
  }

 protected:
  auto RunLoop() -> bool override {
    SetStartupQuiescenceWindowForTest(Duration::zero());

    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(30201);
    OnRegisterCellApp(reg.internal_addr, nullptr, reg);

    cellappmgr::CreateSpaceRequest csr;
    csr.space_id = 302;
    OnCreateSpaceRequest(Address{}, nullptr, csr);

    dirty_before_flush_ =
        GetWatcherRegistry().Get("cellappmgr/ha/snapshot_dirty").value_or("missing");
    OnTickComplete();
    dirty_after_flush_ =
        GetWatcherRegistry().Get("cellappmgr/ha/snapshot_dirty").value_or("missing");
    dirty_reason_after_flush_ =
        GetWatcherRegistry().Get("cellappmgr/ha/snapshot_dirty_reason").value_or("missing");
    saves_after_flush_ =
        GetWatcherRegistry().Get("cellappmgr/ha/snapshot_saves").value_or("missing");
    status_after_flush_ =
        GetWatcherRegistry().Get("cellappmgr/ha/snapshot_status").value_or("missing");
    return true;
  }

 private:
  std::string dirty_before_flush_{"missing"};
  std::string dirty_after_flush_{"missing"};
  std::string dirty_reason_after_flush_{"missing"};
  std::string saves_after_flush_{"missing"};
  std::string status_after_flush_{"missing"};
};

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

auto RequestCellAppStateCount(const RecordingChannel& ch) -> std::size_t {
  std::size_t count = 0;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (id && *id == cellappmgr::RequestCellAppState::Descriptor().id) ++count;
  }
  return count;
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

TEST(CellAppMgr, SnapshotRestore_PreservesTopologyLoadAndNextIds) {
  CellAppMgrHarness h;
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 77;
  csr.initial_cell_count = 2;
  csr.space_master_type = "SpaceMaster";
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(77);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  for (const auto* leaf : leaves) {
    const bool first = leaf->cellapp_addr == MakePeerAddr(30001);
    cellappmgr::InformCellLoad load;
    load.app_id = h.mgr.CellApps().at(leaf->cellapp_addr).app_id;
    load.load = first ? 0.7f : 0.2f;
    load.entity_count = first ? 7u : 2u;
    cellappmgr::InformCellLoad::CellReport rep;
    rep.cell_id = leaf->cell_id;
    rep.entity_count = load.entity_count;
    rep.geometry_version = partition.geometry_version;
    rep.tick_load = load.load;
    rep.script_tick_us = first ? 7000u : 2000u;
    rep.native_tick_us = first ? 1700u : 600u;
    rep.x_buckets = first ? CellLoadBuckets{0, 1, 3, 3, 0, 0, 0, 0}
                          : CellLoadBuckets{0, 0, 0, 0, 1, 1, 0, 0};
    rep.z_buckets = first ? CellLoadBuckets{0, 0, 0, 0, 0, 0, 4, 3}
                          : CellLoadBuckets{0, 0, 1, 1, 0, 0, 0, 0};
    rep.x_load_buckets = first ? CellLoadCostBuckets{0, 100, 300, 300, 0, 0, 0, 0}
                               : CellLoadCostBuckets{0, 0, 0, 0, 100, 100, 0, 0};
    rep.z_load_buckets = first ? CellLoadCostBuckets{0, 0, 0, 0, 0, 0, 400, 300}
                               : CellLoadCostBuckets{0, 0, 100, 100, 0, 0, 0, 0};
    load.cells.push_back(rep);
    h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  }

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();

  ASSERT_EQ(restored.mgr.CellApps().size(), 2u);
  ASSERT_EQ(restored.mgr.Spaces().size(), 1u);
  const auto& restored_partition = restored.mgr.Spaces().at(77);
  EXPECT_EQ(restored_partition.geometry_version, partition.geometry_version);
  EXPECT_EQ(restored_partition.space_master_type, "SpaceMaster");

  const auto restored_leaves = restored_partition.bsp.Leaves();
  ASSERT_EQ(restored_leaves.size(), 2u);
  const auto* first_leaf = restored_partition.bsp.FindCellById(1);
  ASSERT_NE(first_leaf, nullptr);
  EXPECT_EQ(first_leaf->cellapp_addr, MakePeerAddr(30001));
  EXPECT_EQ(first_leaf->entity_count, 7u);
  EXPECT_EQ(first_leaf->script_tick_us, 7000u);
  EXPECT_EQ(first_leaf->native_tick_us, 1700u);
  EXPECT_EQ(first_leaf->x_buckets[2], 3u);
  EXPECT_EQ(first_leaf->z_buckets[6], 4u);
  EXPECT_EQ(first_leaf->x_load_buckets[2], 300u);
  EXPECT_EQ(first_leaf->z_load_buckets[6], 400u);

  cellappmgr::RegisterCellApp reg_c;
  reg_c.internal_addr = MakePeerAddr(30003);
  restored.mgr.OnRegisterCellApp(reg_c.internal_addr, nullptr, reg_c);
  ASSERT_EQ(restored.mgr.CellApps().size(), 3u);
  EXPECT_EQ(restored.mgr.CellApps().at(reg_c.internal_addr).app_id, 3u);
}

TEST(CellAppMgr, SnapshotRestore_PreservesPendingGeometryBroadcast) {
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
  csr.space_id = 88;
  csr.initial_cell_count = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(88);
  const auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 2u);
  const auto b_it = std::find_if(leaves.begin(), leaves.end(), [&](const CellInfo* leaf) {
    return leaf->cellapp_addr == reg_b.internal_addr;
  });
  ASSERT_NE(b_it, leaves.end());
  const auto b_cell_id = (*b_it)->cell_id;

  for (const auto* leaf : leaves) {
    const bool on_b = leaf->cellapp_addr == reg_b.internal_addr;
    cellappmgr::InformCellLoad load;
    load.app_id = h.mgr.CellApps().at(leaf->cellapp_addr).app_id;
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

  const auto app_b = h.mgr.CellApps().at(reg_b.internal_addr).app_id;
  ASSERT_TRUE(h.mgr.GetWatcherRegistry().Set("cellappmgr/lb/retire/app_id",
                                             std::to_string(app_b)));
  h.mgr.TickLoadBalance();
  auto pending = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(*pending, "1");
  auto drains = h.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(drains.has_value());
  EXPECT_EQ(*drains, "1");

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  restored.mgr.RegisterWatchersForTest();
  pending = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(*pending, "1");
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/restore_gate_blocked_pending_geometry"),
            std::optional<std::string>("1"));
  auto gate_status = restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_status");
  ASSERT_TRUE(gate_status.has_value());
  EXPECT_NE(gate_status->find("state=closed"), std::string::npos);
  EXPECT_NE(gate_status->find("blocked_pending_geometry=1"), std::string::npos);
  drains = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(drains.has_value());
  EXPECT_EQ(*drains, "1");

  std::this_thread::sleep_for(std::chrono::milliseconds(550));
  restored.mgr.OnTickCompleteForTest();
  pending = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(*pending, "1");
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/restore_gate_blocked_pending_geometry"),
            std::optional<std::string>("1"));

  cellappmgr::AddCellToSpaceAck ack;
  ack.space_id = 88;
  ack.cell_id = b_cell_id;
  restored.mgr.OnAddCellToSpaceAck(reg_a.internal_addr, nullptr, ack);
  pending = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(*pending, "1");

  restored.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  restored.mgr.OnAddCellToSpaceAck(reg_a.internal_addr, nullptr, ack);
  pending = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/pending_geometry_broadcasts");
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(*pending, "0");
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/restore_gate_blocked_pending_geometry"),
            std::optional<std::string>("0"));
  drains = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(drains.has_value());
  EXPECT_EQ(*drains, "1");

  cellappmgr::InformCellLoad drained;
  drained.app_id = app_b;
  drained.load = 0.1f;
  drained.entity_count = 0;
  cellappmgr::InformCellLoad::CellReport report;
  report.cell_id = b_cell_id;
  report.entity_count = 0;
  report.geometry_version = restored.mgr.Spaces().at(88).geometry_version;
  drained.cells.push_back(report);
  restored.mgr.OnInformCellLoad(reg_b.internal_addr, nullptr, drained);
  drains = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(drains.has_value());
  EXPECT_EQ(*drains, "1");

  restored.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  restored.mgr.OnInformCellLoad(reg_b.internal_addr, nullptr, drained);
  drains = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/drain_count");
  ASSERT_TRUE(drains.has_value());
  EXPECT_EQ(*drains, "0");
  const auto status = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/retire/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find(std::format("app={} owned=0 drains=0 pending=0 ready=1", app_b)),
            std::string::npos);
}

TEST(CellAppMgr, SnapshotRestore_ReattachPreservesAppIdAndReplaysGeometry) {
  CellAppMgrHarness h;
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 99;
  csr.initial_cell_count = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();

  InterfaceTable table;
  RecordingChannel ch_a(restored.dispatcher, table, MakePeerAddr(30001));
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  restored.mgr.OnRegisterCellApp(reg_a.internal_addr, &ch_a, reg_a);

  const auto acks = RegisterCellAppAcks(ch_a);
  ASSERT_FALSE(acks.empty());
  EXPECT_TRUE(acks.back().success);
  EXPECT_EQ(acks.back().app_id, 1u);
  ASSERT_EQ(restored.mgr.CellApps().at(reg_a.internal_addr).channel, &ch_a);

  const auto adds = AddCellMessages(ch_a);
  ASSERT_FALSE(adds.empty());
  EXPECT_EQ(adds.back().space_id, 99u);
  EXPECT_TRUE(adds.back().is_primary);

  const auto updates = UpdateGeometryMessages(ch_a);
  ASSERT_FALSE(updates.empty());
  EXPECT_EQ(updates.back().space_id, 99u);
  EXPECT_EQ(updates.back().geometry_version,
            restored.mgr.Spaces().at(99).geometry_version);
  EXPECT_EQ(RequestCellAppStateCount(ch_a), 1u);
}

TEST(CellAppMgr, SnapshotRestore_UnattachedCellAppsAreNotAssignable) {
  CellAppMgrHarness h;
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  restored.mgr.RegisterWatchersForTest();
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();

  cellappmgr::CreateSpaceRequest before_reattach;
  before_reattach.space_id = 100;
  restored.mgr.OnCreateSpaceRequest(Address{}, nullptr, before_reattach);
  EXPECT_FALSE(restored.mgr.Spaces().contains(100));

  cellappmgr::RegisterCellApp reg_c;
  reg_c.internal_addr = MakePeerAddr(30003);
  restored.mgr.OnRegisterCellApp(reg_c.internal_addr, nullptr, reg_c);

  cellappmgr::CreateSpaceRequest after_reattach;
  after_reattach.space_id = 101;
  restored.mgr.OnCreateSpaceRequest(Address{}, nullptr, after_reattach);

  const auto& partition = restored.mgr.Spaces().at(101);
  const auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, reg_c.internal_addr);

  const auto summary = restored.mgr.GetWatcherRegistry().Get("cellappmgr/lb/cellapps");
  ASSERT_TRUE(summary.has_value());
  EXPECT_NE(summary->find("reattach=1"), std::string::npos);
}

TEST(CellAppMgr, SnapshotRestore_IgnoresLoadUntilReattach) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  restored.mgr.RegisterWatchersForTest();
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();

  auto& partition = restored.mgr.SpacesForTest().at(42);
  const auto cell_id = partition.bsp.PrimaryCellId();
  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.9f;
  load.entity_count = 10;
  load.cells.push_back({cell_id, 10u, 0.f, 0.f, partition.geometry_version});

  restored.mgr.OnInformCellLoad(reg.internal_addr, nullptr, load);
  const auto* leaf = partition.bsp.FindCellById(cell_id);
  ASSERT_NE(leaf, nullptr);
  EXPECT_EQ(restored.mgr.CellApps().at(reg.internal_addr).load, 0.f);
  EXPECT_EQ(leaf->load, 0.f);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("1"));

  restored.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  restored.mgr.OnInformCellLoad(reg.internal_addr, nullptr, load);
  leaf = partition.bsp.FindCellById(cell_id);
  ASSERT_NE(leaf, nullptr);
  EXPECT_FLOAT_EQ(restored.mgr.CellApps().at(reg.internal_addr).load, 0.9f);
  EXPECT_FLOAT_EQ(leaf->load, 0.9f);
}

TEST(CellAppMgr, SnapshotRestore_BlocksLoadBalanceUntilReattachCompletes) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 43;
  csr.initial_cell_count = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto& partition = h.mgr.SpacesForTest().at(43);
  for (const auto* leaf : partition.bsp.Leaves()) {
    const bool on_a = leaf->cellapp_addr == reg_a.internal_addr;
    cellappmgr::InformCellLoad load;
    load.app_id = h.mgr.CellApps().at(leaf->cellapp_addr).app_id;
    load.load = on_a ? 0.9f : 0.1f;
    load.entity_count = on_a ? 900u : 100u;
    load.cells.push_back({leaf->cell_id, load.entity_count, 0.f, 0.f,
                          partition.geometry_version});
    h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  }

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();

  auto& restored_partition = restored.mgr.SpacesForTest().at(43);
  const auto* a_before = restored_partition.bsp.FindCellById(1);
  const auto* b_before = restored_partition.bsp.FindCellById(2);
  ASSERT_NE(a_before, nullptr);
  ASSERT_NE(b_before, nullptr);
  const auto a_bounds = a_before->bounds;
  const auto b_bounds = b_before->bounds;
  const auto version = restored_partition.geometry_version;

  for (int i = 0; i < 5; ++i) restored.mgr.TickLoadBalance();

  const auto* a_pending = restored_partition.bsp.FindCellById(1);
  const auto* b_pending = restored_partition.bsp.FindCellById(2);
  ASSERT_NE(a_pending, nullptr);
  ASSERT_NE(b_pending, nullptr);
  ExpectBoundsEq(a_pending->bounds, a_bounds);
  ExpectBoundsEq(b_pending->bounds, b_bounds);
  EXPECT_EQ(restored_partition.geometry_version, version);

  restored.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  restored.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  for (const auto* leaf : restored_partition.bsp.Leaves()) {
    const bool on_a = leaf->cellapp_addr == reg_a.internal_addr;
    cellappmgr::InformCellLoad load;
    load.app_id = restored.mgr.CellApps().at(leaf->cellapp_addr).app_id;
    load.load = on_a ? 0.9f : 0.1f;
    load.entity_count = on_a ? 900u : 100u;
    load.cells.push_back({leaf->cell_id, load.entity_count, 0.f, 0.f,
                          restored_partition.geometry_version});
    restored.mgr.OnInformCellLoad(Address{}, nullptr, load);
  }

  for (int i = 0; i < 5; ++i) restored.mgr.TickLoadBalance();

  const auto* a_after = restored_partition.bsp.FindCellById(1);
  const auto* b_after = restored_partition.bsp.FindCellById(2);
  ASSERT_NE(a_after, nullptr);
  ASSERT_NE(b_after, nullptr);
  EXPECT_NE(a_after->bounds.max_x, a_bounds.max_x);
  EXPECT_FLOAT_EQ(a_after->bounds.max_x, b_after->bounds.min_x);
}

TEST(CellAppMgr, SnapshotRestore_ReattachRegistryAuditPrunesMissingHostWithoutLeaves) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  restored.mgr.RegisterWatchersForTest();

  const std::vector registry{CellAppProcessInfo(MakePeerAddr(30003), "cellapp_c")};
  restored.mgr.ApplyReattachRegistryAuditForTest(registry);

  EXPECT_TRUE(restored.mgr.CellApps().empty());
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_active"),
            std::optional<std::string>("false"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/reattach_registry_last_missing"),
            std::optional<std::string>("2"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/reattach_registry_last_blocked"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/reattach_registry_reconciled_total"),
            std::optional<std::string>("2"));
}

TEST(CellAppMgr, SnapshotRestore_ReattachRegistryAuditEmptyQueryDoesNotPrune) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  restored.mgr.RegisterWatchersForTest();

  std::vector<machined::ProcessInfo> registry;
  restored.mgr.OnReattachRegistryAuditForTest(std::move(registry));

  EXPECT_TRUE(restored.mgr.CellApps().contains(reg.internal_addr));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("1"));
  auto status = restored.mgr.GetWatcherRegistry().Get(
      "cellappmgr/ha/reattach_registry_status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find("state=error"), std::string::npos);
  EXPECT_NE(status->find("error_detail=cellapp_registry_query_returned_empty"),
            std::string::npos);
}

TEST(CellAppMgr, SnapshotRestore_ReattachRegistryAuditRehomesMissingLeafHost) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 44;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  restored.mgr.RegisterWatchersForTest();
  restored.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  const std::vector registry{CellAppProcessInfo(reg_b.internal_addr, "cellapp_b")};
  restored.mgr.ApplyReattachRegistryAuditForTest(registry);

  EXPECT_FALSE(restored.mgr.CellApps().contains(reg_a.internal_addr));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("0"));
  const auto& partition = restored.mgr.Spaces().at(44);
  const auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, reg_b.internal_addr);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/reattach_registry_last_missing"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/reattach_registry_last_blocked"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/reattach_registry_reconciled_total"),
            std::optional<std::string>("1"));
}

TEST(CellAppMgr, SnapshotRestore_ReattachRegistryAuditBlocksMissingLeafWithoutSurvivor) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 45;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  restored.mgr.RegisterWatchersForTest();

  const std::vector registry{CellAppProcessInfo(MakePeerAddr(30002), "cellapp_b")};
  restored.mgr.ApplyReattachRegistryAuditForTest(registry);

  EXPECT_TRUE(restored.mgr.CellApps().contains(reg.internal_addr));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_active"),
            std::optional<std::string>("true"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/reattach_registry_last_missing"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/reattach_registry_last_blocked"),
            std::optional<std::string>("1"));
  auto status = restored.mgr.GetWatcherRegistry().Get(
      "cellappmgr/ha/reattach_registry_status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find("state=blocked"), std::string::npos);
}

TEST(CellAppMgr, SnapshotRestore_WatchersTrackReattachConvergence) {
  CellAppMgrHarness h;
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }
  h.mgr.RegisterWatchersForTest();
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_state"),
            std::optional<std::string>("idle"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_active"),
            std::optional<std::string>("false"));
  auto gate_status = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_status");
  ASSERT_TRUE(gate_status.has_value());
  EXPECT_NE(gate_status->find("state=open"), std::string::npos);

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  restored.mgr.RegisterWatchersForTest();

  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restored_cellapps"),
            std::optional<std::string>("2"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("2"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_completed_count"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_stuck"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_completed"),
            std::optional<std::string>("false"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_state"),
            std::optional<std::string>("pending"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_active"),
            std::optional<std::string>("true"));
  gate_status = restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_status");
  ASSERT_TRUE(gate_status.has_value());
  EXPECT_NE(gate_status->find("state=closed"), std::string::npos);
  EXPECT_NE(gate_status->find("pending_reattach=2"), std::string::npos);
  auto status = restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find("state=pending"), std::string::npos);
  EXPECT_NE(status->find("restored=2 pending=2 completed=0"), std::string::npos);
  EXPECT_NE(status->find("completed_count=0"), std::string::npos);
  EXPECT_NE(status->find("app=1"), std::string::npos);

  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  restored.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_completed_count"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_active"),
            std::optional<std::string>("true"));

  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  restored.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_completed"),
            std::optional<std::string>("true"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_state"),
            std::optional<std::string>("complete"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_active"),
            std::optional<std::string>("false"));
  gate_status = restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/restore_gate_status");
  ASSERT_TRUE(gate_status.has_value());
  EXPECT_NE(gate_status->find("state=open"), std::string::npos);
  EXPECT_NE(gate_status->find("pending_reattach=0"), std::string::npos);
  status = restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find("state=complete"), std::string::npos);
  EXPECT_NE(status->find("restored=2 pending=0 completed=1"), std::string::npos);
  EXPECT_NE(status->find("completed_count=2"), std::string::npos);
  EXPECT_NE(status->find("state=attached"), std::string::npos);
}

TEST(CellAppMgr, SnapshotRestore_WatchersMarkReattachStuckAfterWatchdog) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  const auto snapshot = h.mgr.Snapshot();
  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(snapshot);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  restored.mgr.RegisterWatchersForTest();
  ASSERT_TRUE(restored.mgr.GetWatcherRegistry().Set("cellappmgr/ha/reattach_watchdog_ms", "0"));

  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_pending"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_stuck"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_state"),
            std::optional<std::string>("stuck"));
  auto status = restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/reattach_status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find("state=stuck"), std::string::npos);
  EXPECT_NE(status->find("restored=1 pending=1 completed=0 stuck=1"), std::string::npos);
  EXPECT_NE(status->find("completed_count=0"), std::string::npos);
}

TEST(CellAppMgr, SnapshotFileRoundTripRestoresTopology) {
  CellAppMgrHarness h;
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 102;
  csr.initial_cell_count = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  h.mgr.RegisterWatchersForTest();

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_snapshot_{}_{}.bin", 102, stamp);
  ASSERT_TRUE(h.mgr.SaveSnapshotToFile(path).HasValue());
  auto attempt_age = WatcherInt64ForTest(h.mgr.GetWatcherRegistry(),
                                         "cellappmgr/ha/snapshot_last_save_attempt_age_ms");
  ASSERT_TRUE(attempt_age.has_value());
  EXPECT_GE(*attempt_age, 0);
  auto save_age = WatcherInt64ForTest(h.mgr.GetWatcherRegistry(),
                                      "cellappmgr/ha/snapshot_last_save_age_ms");
  ASSERT_TRUE(save_age.has_value());
  EXPECT_GE(*save_age, 0);
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_save_path"),
            std::optional<std::string>(path.string()));
  auto save_topology =
      h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_save_topology");
  ASSERT_TRUE(save_topology.has_value());
  EXPECT_NE(save_topology->find("spaces=1"), std::string::npos);
  EXPECT_NE(save_topology->find("space=102"), std::string::npos);
  EXPECT_NE(save_topology->find("leaves=2"), std::string::npos);
  EXPECT_NE(save_topology->find("pending_ack=0"), std::string::npos);
  EXPECT_EQ(save_topology->find("load="), std::string::npos);
  EXPECT_EQ(
      h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_save_topology_pending_ack"),
      std::optional<std::string>("0"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_save_error"),
            std::optional<std::string>(""));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_stale"),
            std::optional<std::string>("false"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_saves"),
            std::optional<std::string>("1"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_file_present"),
            std::optional<std::string>("true"));
  auto file_bytes =
      WatcherInt64ForTest(h.mgr.GetWatcherRegistry(), "cellappmgr/ha/snapshot_file_bytes");
  ASSERT_TRUE(file_bytes.has_value());
  EXPECT_GT(*file_bytes, 0);
  auto file_status = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_file_status");
  ASSERT_TRUE(file_status.has_value());
  EXPECT_NE(file_status->find("state=ready"), std::string::npos);
  EXPECT_NE(file_status->find("present=1"), std::string::npos);
  EXPECT_NE(file_status->find("valid=1"), std::string::npos);
  EXPECT_NE(file_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(file_status->find("error_detail=none"), std::string::npos);
  auto file_topology_status =
      h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_file_topology_status");
  ASSERT_TRUE(file_topology_status.has_value());
  EXPECT_NE(file_topology_status->find("state=ready"), std::string::npos);
  EXPECT_NE(file_topology_status->find("restorable=1"), std::string::npos);
  EXPECT_NE(file_topology_status->find("topology_present=1"), std::string::npos);
  EXPECT_NE(file_topology_status->find("topology_pending_ack=0"), std::string::npos);
  EXPECT_NE(file_topology_status->find("matches_expected=1"), std::string::npos);
  EXPECT_NE(file_topology_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(file_topology_status->find("error_detail=none"), std::string::npos);
  auto save_status = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_status");
  ASSERT_TRUE(save_status.has_value());
  EXPECT_NE(save_status->find("state=disabled"), std::string::npos);
  EXPECT_NE(save_status->find("saves=1"), std::string::npos);
  EXPECT_NE(save_status->find("topology_present=1"), std::string::npos);
  EXPECT_NE(save_status->find("topology_pending_ack=0"), std::string::npos);
  EXPECT_NE(save_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(save_status->find("error_detail=none"), std::string::npos);

  CellAppMgrHarness restored;
  restored.mgr.RegisterWatchersForTest();
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  ASSERT_TRUE(restored.mgr.Spaces().contains(102));
  EXPECT_EQ(restored.mgr.Spaces().at(102).bsp.Leaves().size(), 2u);
  EXPECT_TRUE(restored.mgr.CellApps().at(MakePeerAddr(30001)).needs_reattach);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_source"),
            std::optional<std::string>("primary"));
  auto restore_status =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_status");
  ASSERT_TRUE(restore_status.has_value());
  EXPECT_NE(restore_status->find("state=primary"), std::string::npos);
  EXPECT_NE(restore_status->find("source=primary"), std::string::npos);
  EXPECT_NE(restore_status->find("restores=1"), std::string::npos);
  EXPECT_NE(restore_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("error_detail=none"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_detail=none"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_present=1"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_pending_ack=0"), std::string::npos);
  auto restore_attempt_age =
      WatcherInt64ForTest(restored.mgr.GetWatcherRegistry(),
                          "cellappmgr/ha/snapshot_last_restore_attempt_age_ms");
  ASSERT_TRUE(restore_attempt_age.has_value());
  EXPECT_GE(*restore_attempt_age, 0);
  auto restore_age = WatcherInt64ForTest(restored.mgr.GetWatcherRegistry(),
                                         "cellappmgr/ha/snapshot_last_restore_age_ms");
  ASSERT_TRUE(restore_age.has_value());
  EXPECT_GE(*restore_age, 0);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_path"),
            std::optional<std::string>(path.string()));
  auto restore_topology =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_topology");
  ASSERT_TRUE(restore_topology.has_value());
  EXPECT_NE(restore_topology->find("spaces=1"), std::string::npos);
  EXPECT_NE(restore_topology->find("space=102"), std::string::npos);
  EXPECT_NE(restore_topology->find("leaves=2"), std::string::npos);
  EXPECT_NE(restore_topology->find("pending_ack=0"), std::string::npos);
  EXPECT_EQ(restore_topology->find("load="), std::string::npos);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/snapshot_last_restore_topology_pending_ack"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_error"),
            std::optional<std::string>(""));
  EXPECT_EQ(
      restored.mgr.GetWatcherRegistry().Get(
          "cellappmgr/ha/snapshot_last_restore_primary_error"),
      std::optional<std::string>(""));
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(CellAppMgr, SnapshotFileRestoreFallsBackToBackup) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_snapshot_fallback_{}.bin", stamp);
  auto backup_path = path;
  backup_path += ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);
  h.mgr.RegisterWatchersForTest();

  cellappmgr::CreateSpaceRequest first;
  first.space_id = 201;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, first);
  ASSERT_TRUE(h.mgr.SaveSnapshotToFile(path).HasValue());
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_backup_path"),
            std::optional<std::string>(backup_path.string()));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_backup_present"),
            std::optional<std::string>("false"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_backup_bytes"),
            std::optional<std::string>("0"));
  auto backup_status = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_backup_status");
  ASSERT_TRUE(backup_status.has_value());
  EXPECT_NE(backup_status->find("state=missing"), std::string::npos);
  EXPECT_NE(backup_status->find("present=0"), std::string::npos);
  EXPECT_NE(backup_status->find("bytes=0"), std::string::npos);
  EXPECT_NE(backup_status->find("valid=0"), std::string::npos);

  cellappmgr::CreateSpaceRequest second;
  second.space_id = 202;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, second);
  ASSERT_TRUE(h.mgr.SaveSnapshotToFile(path).HasValue());
  ASSERT_TRUE(fs::Exists(backup_path));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_backup_present"),
            std::optional<std::string>("true"));
  auto backup_bytes =
      WatcherInt64ForTest(h.mgr.GetWatcherRegistry(), "cellappmgr/ha/snapshot_backup_bytes");
  ASSERT_TRUE(backup_bytes.has_value());
  EXPECT_GT(*backup_bytes, 0);
  backup_status = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_backup_status");
  ASSERT_TRUE(backup_status.has_value());
  EXPECT_NE(backup_status->find("state=ready"), std::string::npos);
  EXPECT_NE(backup_status->find("present=1"), std::string::npos);
  EXPECT_NE(backup_status->find("valid=1"), std::string::npos);
  EXPECT_NE(backup_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(backup_status->find("error_detail=none"), std::string::npos);
  auto backup_topology_status =
      h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_backup_topology_status");
  ASSERT_TRUE(backup_topology_status.has_value());
  EXPECT_NE(backup_topology_status->find("state=ready"), std::string::npos);
  EXPECT_NE(backup_topology_status->find("restorable=1"), std::string::npos);
  EXPECT_NE(backup_topology_status->find("topology_present=1"), std::string::npos);
  EXPECT_NE(backup_topology_status->find("topology_pending_ack=0"), std::string::npos);
  EXPECT_NE(backup_topology_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(backup_topology_status->find("error_detail=none"), std::string::npos);

  ASSERT_TRUE(fs::WriteTextFile(path, "corrupt").HasValue());
  auto corrupt_status = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_file_status");
  ASSERT_TRUE(corrupt_status.has_value());
  EXPECT_NE(corrupt_status->find("state=invalid"), std::string::npos);
  EXPECT_NE(corrupt_status->find("valid=0"), std::string::npos);
  EXPECT_NE(corrupt_status->find("error_present=1"), std::string::npos);
  EXPECT_NE(corrupt_status->find("error_detail=CellAppMgr_snapshot_header"),
            std::string::npos);
  auto corrupt_topology_status =
      h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_file_topology_status");
  ASSERT_TRUE(corrupt_topology_status.has_value());
  EXPECT_NE(corrupt_topology_status->find("state=invalid"), std::string::npos);
  EXPECT_NE(corrupt_topology_status->find("restorable=0"), std::string::npos);
  EXPECT_NE(corrupt_topology_status->find("topology_present=0"), std::string::npos);
  EXPECT_NE(corrupt_topology_status->find("error_present=1"), std::string::npos);
  EXPECT_NE(corrupt_topology_status->find("error_detail=CellAppMgr_snapshot_header"),
            std::string::npos);

  CellAppMgrHarness restored;
  restored.mgr.RegisterWatchersForTest();
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  EXPECT_TRUE(restored.mgr.Spaces().contains(201));
  EXPECT_FALSE(restored.mgr.Spaces().contains(202));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restores"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_fallback_restores"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_source"),
            std::optional<std::string>("backup"));
  auto restore_status =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_status");
  ASSERT_TRUE(restore_status.has_value());
  EXPECT_NE(restore_status->find("state=fallback"), std::string::npos);
  EXPECT_NE(restore_status->find("source=backup"), std::string::npos);
  EXPECT_NE(restore_status->find("fallback_restores=1"), std::string::npos);
  EXPECT_NE(restore_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("error_detail=none"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_present=1"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_detail=CellAppMgr_snapshot_header"),
            std::string::npos);
  EXPECT_NE(restore_status->find("topology_present=1"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_pending_ack=0"), std::string::npos);
  auto restore_attempt_age =
      WatcherInt64ForTest(restored.mgr.GetWatcherRegistry(),
                          "cellappmgr/ha/snapshot_last_restore_attempt_age_ms");
  ASSERT_TRUE(restore_attempt_age.has_value());
  EXPECT_GE(*restore_attempt_age, 0);
  auto restore_age = WatcherInt64ForTest(restored.mgr.GetWatcherRegistry(),
                                         "cellappmgr/ha/snapshot_last_restore_age_ms");
  ASSERT_TRUE(restore_age.has_value());
  EXPECT_GE(*restore_age, 0);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_path"),
            std::optional<std::string>(backup_path.string()));
  auto restore_topology =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_topology");
  ASSERT_TRUE(restore_topology.has_value());
  EXPECT_NE(restore_topology->find("space=201"), std::string::npos);
  EXPECT_EQ(restore_topology->find("space=202"), std::string::npos);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/snapshot_last_restore_topology_pending_ack"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_error"),
            std::optional<std::string>(""));
  auto primary_error =
      restored.mgr.GetWatcherRegistry().Get(
          "cellappmgr/ha/snapshot_last_restore_primary_error");
  ASSERT_TRUE(primary_error.has_value());
  EXPECT_NE(primary_error->find("header truncated"), std::string::npos);

  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);
}

TEST(CellAppMgr, SnapshotFileRestoreFallsBackWhenPrimaryMissing) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_snapshot_backup_only_{}.bin", stamp);
  auto backup_path = path;
  backup_path += ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);

  cellappmgr::CreateSpaceRequest req;
  req.space_id = 203;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, req);
  ASSERT_TRUE(h.mgr.SaveSnapshotToFile(path).HasValue());
  std::filesystem::rename(path, backup_path, ec);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_FALSE(fs::Exists(path));
  EXPECT_TRUE(fs::Exists(backup_path));

  CellAppMgrHarness restored;
  restored.mgr.RegisterWatchersForTest();
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  EXPECT_TRUE(restored.mgr.Spaces().contains(203));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restores"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_fallback_restores"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_source"),
            std::optional<std::string>("backup"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_path"),
            std::optional<std::string>(backup_path.string()));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_error"),
            std::optional<std::string>(""));
  auto primary_error =
      restored.mgr.GetWatcherRegistry().Get(
          "cellappmgr/ha/snapshot_last_restore_primary_error");
  ASSERT_TRUE(primary_error.has_value());
  EXPECT_NE(primary_error->find("snapshot file not found"), std::string::npos);
  auto restore_status =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_status");
  ASSERT_TRUE(restore_status.has_value());
  EXPECT_NE(restore_status->find("state=fallback"), std::string::npos);
  EXPECT_NE(restore_status->find("source=backup"), std::string::npos);
  EXPECT_NE(restore_status->find("fallback_restores=1"), std::string::npos);
  EXPECT_NE(restore_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("error_detail=none"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_present=1"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_detail=snapshot_file_not_found"),
            std::string::npos);
  EXPECT_NE(restore_status->find("topology_present=1"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_pending_ack=0"), std::string::npos);
  auto restore_topology =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_topology");
  ASSERT_TRUE(restore_topology.has_value());
  EXPECT_NE(restore_topology->find("space=203"), std::string::npos);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/snapshot_last_restore_topology_pending_ack"),
            std::optional<std::string>("0"));

  std::filesystem::remove(backup_path, ec);
}

TEST(CellAppMgr, SnapshotFileRestoreFailsWhenPrimaryMissingAndBackupCorrupt) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_snapshot_missing_{}.bin", stamp);
  auto backup_path = path;
  backup_path += ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);

  ASSERT_TRUE(fs::WriteTextFile(backup_path, "corrupt").HasValue());

  CellAppMgrHarness restored;
  restored.mgr.RegisterWatchersForTest();
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_FALSE(restore.HasValue());
  EXPECT_EQ(restore.Error().Code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(std::string(restore.Error().Message()).find("header truncated"),
            std::string::npos);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restores"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_fallback_restores"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_failures"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_failures"),
            std::optional<std::string>("1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_source"),
            std::optional<std::string>("none"));
  auto restore_status =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_status");
  ASSERT_TRUE(restore_status.has_value());
  EXPECT_NE(restore_status->find("state=failed"), std::string::npos);
  EXPECT_NE(restore_status->find("restore_failures=1"), std::string::npos);
  EXPECT_NE(restore_status->find("error_present=1"), std::string::npos);
  EXPECT_NE(restore_status->find("error_detail=CellAppMgr_snapshot_header"),
            std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_present=1"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_detail=snapshot_file_not_found"),
            std::string::npos);
  EXPECT_NE(restore_status->find("topology_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_pending_ack=0"), std::string::npos);
  auto restore_attempt_age =
      WatcherInt64ForTest(restored.mgr.GetWatcherRegistry(),
                          "cellappmgr/ha/snapshot_last_restore_attempt_age_ms");
  ASSERT_TRUE(restore_attempt_age.has_value());
  EXPECT_GE(*restore_attempt_age, 0);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_age_ms"),
            std::optional<std::string>("-1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_path"),
            std::optional<std::string>(""));
  EXPECT_EQ(
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_topology"),
      std::optional<std::string>(""));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/snapshot_last_restore_topology_pending_ack"),
            std::optional<std::string>("0"));
  auto last_error =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_error");
  ASSERT_TRUE(last_error.has_value());
  EXPECT_NE(last_error->find("header truncated"), std::string::npos);
  auto primary_error =
      restored.mgr.GetWatcherRegistry().Get(
          "cellappmgr/ha/snapshot_last_restore_primary_error");
  ASSERT_TRUE(primary_error.has_value());
  EXPECT_NE(primary_error->find("snapshot file not found"), std::string::npos);

  std::filesystem::remove(backup_path, ec);
}

TEST(CellAppMgr, SnapshotFileRestoreMissingPrimaryAndBackupDoesNotCountFailure) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_snapshot_absent_{}.bin", stamp);
  auto backup_path = path;
  backup_path += ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);

  CellAppMgrHarness restored;
  restored.mgr.RegisterWatchersForTest();
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_FALSE(restore.HasValue());
  EXPECT_EQ(restore.Error().Code(), ErrorCode::kNotFound);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restores"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_source"),
            std::optional<std::string>("none"));
  auto restore_status =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_status");
  ASSERT_TRUE(restore_status.has_value());
  EXPECT_NE(restore_status->find("state=missing"), std::string::npos);
  EXPECT_NE(restore_status->find("restored=0"), std::string::npos);
  EXPECT_NE(restore_status->find("error_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("error_detail=none"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("primary_error_detail=none"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_pending_ack=0"), std::string::npos);
  auto restore_attempt_age =
      WatcherInt64ForTest(restored.mgr.GetWatcherRegistry(),
                          "cellappmgr/ha/snapshot_last_restore_attempt_age_ms");
  ASSERT_TRUE(restore_attempt_age.has_value());
  EXPECT_GE(*restore_attempt_age, 0);
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_age_ms"),
            std::optional<std::string>("-1"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_path"),
            std::optional<std::string>(""));
  EXPECT_EQ(
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_topology"),
      std::optional<std::string>(""));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get(
                "cellappmgr/ha/snapshot_last_restore_topology_pending_ack"),
            std::optional<std::string>("0"));
  EXPECT_EQ(restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_error"),
            std::optional<std::string>(""));
  EXPECT_EQ(
      restored.mgr.GetWatcherRegistry().Get(
          "cellappmgr/ha/snapshot_last_restore_primary_error"),
      std::optional<std::string>(""));
}

TEST(CellAppMgr, SnapshotFileRestoreFailureClearsPreviousRestoreTopology) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest req;
  req.space_id = 204;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, req);

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_snapshot_restore_retry_{}.bin", stamp);
  auto backup_path = path;
  backup_path += ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);
  ASSERT_TRUE(h.mgr.SaveSnapshotToFile(path).HasValue());

  CellAppMgrHarness restored;
  restored.mgr.RegisterWatchersForTest();
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  auto restore_topology =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_topology");
  ASSERT_TRUE(restore_topology.has_value());
  EXPECT_NE(restore_topology->find("space=204"), std::string::npos);

  ASSERT_TRUE(fs::WriteTextFile(path, "corrupt").HasValue());
  std::filesystem::remove(backup_path, ec);
  restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_FALSE(restore.HasValue());
  EXPECT_EQ(
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_restore_topology"),
      std::optional<std::string>(""));
  auto restore_status =
      restored.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_status");
  ASSERT_TRUE(restore_status.has_value());
  EXPECT_NE(restore_status->find("state=failed"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_present=0"), std::string::npos);
  EXPECT_NE(restore_status->find("topology_pending_ack=0"), std::string::npos);

  std::filesystem::remove(path, ec);
}

TEST(CellAppMgr, SaveSnapshotToFileRecordsLastSaveError) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_snapshot_dir_{}", stamp);
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  ASSERT_TRUE(fs::CreateDirectories(path).HasValue());

  CellAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  auto save = h.mgr.SaveSnapshotToFile(path);
  ASSERT_FALSE(save.HasValue());
  EXPECT_EQ(save.Error().Code(), ErrorCode::kIoError);
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_save_path"),
            std::optional<std::string>(path.string()));
  auto attempt_age = WatcherInt64ForTest(h.mgr.GetWatcherRegistry(),
                                         "cellappmgr/ha/snapshot_last_save_attempt_age_ms");
  ASSERT_TRUE(attempt_age.has_value());
  EXPECT_GE(*attempt_age, 0);
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_save_age_ms"),
            std::optional<std::string>("-1"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_failures"),
            std::optional<std::string>("1"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_failures"),
            std::optional<std::string>("1"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_stale"),
            std::optional<std::string>("false"));
  auto status = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find("state=disabled"), std::string::npos);
  EXPECT_NE(status->find("save_failures=1"), std::string::npos);
  EXPECT_NE(status->find("error_present=1"), std::string::npos);
  EXPECT_NE(status->find("error_detail="), std::string::npos);
  EXPECT_EQ(status->find("error_detail=none"), std::string::npos);
  auto last_error = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_save_error");
  ASSERT_TRUE(last_error.has_value());
  EXPECT_FALSE(last_error->empty());
  EXPECT_EQ(*last_error, std::string(save.Error().Message()));

  std::filesystem::remove_all(path, ec);
}

TEST(CellAppMgr, SaveSnapshotToFileSkipsBackupWhenMainFileCorrupt) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_backup_skip_{}.bin", stamp);
  auto backup_path = path;
  backup_path += ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);

  ASSERT_TRUE(fs::WriteTextFile(path, "corrupt-existing-main-file").HasValue());

  CellAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  auto save = h.mgr.SaveSnapshotToFile(path);
  ASSERT_TRUE(save.HasValue());

  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_backup_skips"),
            std::optional<std::string>("1"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_failures"),
            std::optional<std::string>("0"));
  EXPECT_TRUE(std::filesystem::exists(path));

  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);
}

TEST(CellAppMgr, SnapshotSizeHighWaterPctReflectsLastSaveBytes) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_size_water_{}.bin", stamp);
  std::error_code ec;
  std::filesystem::remove(path, ec);

  CellAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_size_high_water_pct"),
            std::optional<std::string>("0"));
  auto save = h.mgr.SaveSnapshotToFile(path);
  ASSERT_TRUE(save.HasValue());
  auto pct = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_size_high_water_pct");
  ASSERT_TRUE(pct.has_value());
  EXPECT_EQ(*pct, std::string("0"));  // tiny snapshot vs 16 MiB cap → rounds to 0%
  auto max_bytes = h.mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_max_bytes");
  ASSERT_TRUE(max_bytes.has_value());
  EXPECT_NE(*max_bytes, std::string("0"));

  std::filesystem::remove(path, ec);
}

TEST(CellAppMgr, PeriodicSnapshotFailuresRespectInterval) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_periodic_snapshot_dir_{}", stamp);
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  EventDispatcher dispatcher{"cellappmgr-periodic-snapshot-failure"};
  NetworkInterface network{dispatcher};
  FailingPeriodicSnapshotCellAppMgr mgr{dispatcher, network};
  std::vector<std::string> args{
      "atlas_cellappmgr_test",
      "--type",
      "cellappmgr",
      "--name",
      "cellappmgr_periodic_snapshot_failure",
      "--internal-port",
      "0",
      "--machined",
      "127.0.0.1:9",
      "--snapshot-path",
      path.string(),
      "--snapshot-interval-ms",
      "60000",
      "--log-level",
      "critical",
      "--update-hertz",
      "100"};
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (auto& arg : args) argv.push_back(arg.data());

  ASSERT_EQ(mgr.RunApp(static_cast<int>(argv.size()), argv.data()), 0);
  EXPECT_EQ(mgr.PeriodicSnapshotFailuresForTest(), "1");
  auto attempt_age = WatcherInt64ForTest(mgr.GetWatcherRegistry(),
                                         "cellappmgr/ha/snapshot_last_save_attempt_age_ms");
  ASSERT_TRUE(attempt_age.has_value());
  EXPECT_GE(*attempt_age, 0);
  EXPECT_EQ(mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_last_save_age_ms"),
            std::optional<std::string>("-1"));
  EXPECT_EQ(mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_failures"),
            std::optional<std::string>("2"));
  EXPECT_EQ(mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_restore_failures"),
            std::optional<std::string>("0"));
  EXPECT_EQ(mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_failures"),
            std::optional<std::string>("2"));
  EXPECT_EQ(mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_stale"),
            std::optional<std::string>("true"));
  auto status = mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find("state=stale"), std::string::npos);
  EXPECT_NE(status->find("saves=0"), std::string::npos);
  EXPECT_NE(status->find("save_failures=2"), std::string::npos);
  EXPECT_NE(status->find("error_present=1"), std::string::npos);
  EXPECT_NE(status->find("error_detail="), std::string::npos);
  EXPECT_EQ(status->find("error_detail=none"), std::string::npos);

  std::filesystem::remove_all(path, ec);
}

TEST(CellAppMgr, DirtyTopologySnapshotFlushesBeforePeriodicInterval) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_dirty_snapshot_{}.bin", stamp);
  std::error_code ec;
  std::filesystem::remove(path, ec);

  EventDispatcher dispatcher{"cellappmgr-dirty-snapshot"};
  NetworkInterface network{dispatcher};
  DirtySnapshotCellAppMgr mgr{dispatcher, network};
  std::vector<std::string> args{
      "atlas_cellappmgr_test",
      "--type",
      "cellappmgr",
      "--name",
      "cellappmgr_dirty_snapshot",
      "--internal-port",
      "0",
      "--machined",
      "127.0.0.1:9",
      "--snapshot-path",
      path.string(),
      "--snapshot-interval-ms",
      "60000",
      "--log-level",
      "critical",
      "--update-hertz",
      "100"};
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (auto& arg : args) argv.push_back(arg.data());

  ASSERT_EQ(mgr.RunApp(static_cast<int>(argv.size()), argv.data()), 0);
  EXPECT_EQ(mgr.DirtyBeforeFlushForTest(), "true");
  EXPECT_EQ(mgr.DirtyAfterFlushForTest(), "false");
  EXPECT_TRUE(mgr.DirtyReasonAfterFlushForTest().empty());
  EXPECT_EQ(mgr.SavesAfterFlushForTest(), "1");
  EXPECT_NE(mgr.StatusAfterFlushForTest().find("state=healthy"), std::string::npos);
  EXPECT_NE(mgr.StatusAfterFlushForTest().find("dirty=0"), std::string::npos);
  EXPECT_NE(mgr.StatusAfterFlushForTest().find("dirty_reason=none"), std::string::npos);
  EXPECT_TRUE(fs::Exists(path));

  CellAppMgrHarness restored;
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  ASSERT_TRUE(restored.mgr.Spaces().contains(302));
  EXPECT_TRUE(restored.mgr.CellApps().contains(MakePeerAddr(30201)));
  std::filesystem::remove(path, ec);
}

TEST(CellAppMgr, ShutdownSavesConfiguredSnapshot) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_shutdown_snapshot_{}.bin", stamp);
  std::error_code ec;
  std::filesystem::remove(path, ec);

  EventDispatcher dispatcher{"cellappmgr-shutdown-snapshot"};
  NetworkInterface network{dispatcher};
  ShutdownSnapshotCellAppMgr mgr{dispatcher, network};
  std::vector<std::string> args{
      "atlas_cellappmgr_test",
      "--type",
      "cellappmgr",
      "--name",
      "cellappmgr_shutdown_snapshot",
      "--internal-port",
      "0",
      "--machined",
      "127.0.0.1:9",
      "--snapshot-path",
      path.string(),
      "--snapshot-interval-ms",
      "0",
      "--log-level",
      "critical",
      "--update-hertz",
      "100"};
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (auto& arg : args) argv.push_back(arg.data());

  ASSERT_EQ(mgr.RunApp(static_cast<int>(argv.size()), argv.data()), 0);
  ASSERT_TRUE(fs::Exists(path));
  EXPECT_EQ(mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_saves"),
            std::optional<std::string>("1"));
  auto attempt_age = WatcherInt64ForTest(mgr.GetWatcherRegistry(),
                                         "cellappmgr/ha/snapshot_last_save_attempt_age_ms");
  ASSERT_TRUE(attempt_age.has_value());
  EXPECT_GE(*attempt_age, 0);
  auto save_age = WatcherInt64ForTest(mgr.GetWatcherRegistry(),
                                      "cellappmgr/ha/snapshot_last_save_age_ms");
  ASSERT_TRUE(save_age.has_value());
  EXPECT_GE(*save_age, 0);
  EXPECT_EQ(mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_save_stale"),
            std::optional<std::string>("false"));
  auto status = mgr.GetWatcherRegistry().Get("cellappmgr/ha/snapshot_status");
  ASSERT_TRUE(status.has_value());
  EXPECT_NE(status->find("state=healthy"), std::string::npos);
  EXPECT_NE(status->find("saves=1"), std::string::npos);
  EXPECT_NE(status->find("error_present=0"), std::string::npos);
  EXPECT_NE(status->find("error_detail=none"), std::string::npos);

  CellAppMgrHarness restored;
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
  ASSERT_TRUE(restored.mgr.Spaces().contains(301));
  EXPECT_TRUE(restored.mgr.CellApps().contains(MakePeerAddr(30101)));
  std::filesystem::remove(path, ec);
}

TEST(CellAppMgr, SnapshotRestore_RejectsUnsupportedVersion) {
  CellAppMgrHarness h;
  const auto snapshot = h.mgr.Snapshot();
  auto corrupted = snapshot;
  ASSERT_GT(corrupted.size(), 8u);
  corrupted[4] = std::byte{99};
  corrupted[5] = std::byte{0};
  corrupted[6] = std::byte{0};
  corrupted[7] = std::byte{0};

  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(corrupted);
  ASSERT_FALSE(restore.HasValue());
  EXPECT_EQ(restore.Error().Code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(std::string(restore.Error().Message()).find("unsupported version"),
            std::string::npos);
}

TEST(CellAppMgr, SnapshotRestore_RejectsChecksumMismatch) {
  CellAppMgrHarness h;
  const auto snapshot = h.mgr.Snapshot();
  auto corrupted = snapshot;
  ASSERT_GT(corrupted.size(), 24u);
  corrupted.back() ^= std::byte{0x01};

  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(corrupted);
  ASSERT_FALSE(restore.HasValue());
  EXPECT_EQ(restore.Error().Code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(std::string(restore.Error().Message()).find("checksum"), std::string::npos);
}

TEST(CellAppMgr, SnapshotRestore_RejectsDuplicateAppIds) {
  CellAppMgrHarness h;
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }

  auto payload = SnapshotPayloadForTest(h.mgr.Snapshot());
  ASSERT_FALSE(payload.empty());
  const auto first = CellAppRecordOffsetForTest(payload, 0);
  const auto second = CellAppRecordOffsetForTest(payload, 1);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  constexpr std::size_t app_id_offset = sizeof(uint32_t) + sizeof(uint16_t);
  const auto first_app_id =
      ReadLittleAtForTest<uint32_t>(payload, *first + app_id_offset);
  PatchLittleAtForTest<uint32_t>(payload, *second + app_id_offset, first_app_id);
  const auto corrupted = SnapshotWithPayloadForTest(
      std::span<const std::byte>(payload.data(), payload.size()));

  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(corrupted);
  ASSERT_FALSE(restore.HasValue());
  EXPECT_EQ(restore.Error().Code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(std::string(restore.Error().Message()).find("duplicate app_id"),
            std::string::npos);
}

TEST(CellAppMgr, SnapshotRestore_RejectsUnknownLeafCellApp) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 121;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto payload = SnapshotPayloadForTest(h.mgr.Snapshot());
  ASSERT_FALSE(payload.empty());
  const auto record = CellAppRecordOffsetForTest(payload, 0);
  ASSERT_TRUE(record.has_value());
  PatchLittleAtForTest<uint16_t>(payload, *record + sizeof(uint32_t), 31001);
  const auto corrupted = SnapshotWithPayloadForTest(
      std::span<const std::byte>(payload.data(), payload.size()));

  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(corrupted);
  ASSERT_FALSE(restore.HasValue());
  EXPECT_EQ(restore.Error().Code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(std::string(restore.Error().Message()).find("unknown leaf cellapp"),
            std::string::npos);
}

TEST(CellAppMgr, RestoreSnapshotFromFileRejectsCorruptBytes) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_cellappmgr_snapshot_corrupt_{}.bin", stamp);
  ASSERT_TRUE(fs::WriteTextFile(path, "corrupt").HasValue());

  CellAppMgrHarness restored;
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_FALSE(restore.HasValue());
  EXPECT_EQ(restore.Error().Code(), ErrorCode::kInvalidArgument);

  std::error_code ec;
  std::filesystem::remove(path, ec);
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

  CellAppMgrHarness restored;
  auto restore = restored.mgr.Restore(h.mgr.Snapshot());
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
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

}  // namespace
}  // namespace atlas
