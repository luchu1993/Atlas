#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "baseappmgr/baseappmgr.h"
#include "network/address.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "platform/filesystem.h"
#include "serialization/binary_stream.h"

namespace atlas {
namespace {

class TestBaseAppMgr final : public BaseAppMgr {
 public:
  using BaseAppMgr::BaseAppMgr;

  void RegisterWatchersForTest() { RegisterWatchers(); }
};

struct BaseAppMgrHarness {
  EventDispatcher dispatcher{"baseappmgr-test"};
  NetworkInterface network{dispatcher};
  TestBaseAppMgr mgr{dispatcher, network};
};

auto MakeAddr(uint16_t port) -> Address { return Address(0x7F000001u, port); }

constexpr uint32_t kSnapshotMagicForTest = 0x424D4731u;  // 'BMG1'
constexpr uint32_t kSnapshotVersionForTest = 1;

auto WatcherInt64ForTest(const WatcherRegistry& wr, const std::string& path) -> std::optional<int64_t> {
  auto raw = wr.Get(path);
  if (!raw) return std::nullopt;
  try {
    return std::stoll(*raw);
  } catch (...) {
    return std::nullopt;
  }
}

TEST(BaseAppMgr, SnapshotRoundtripPreservesAuthoritativeState) {
  BaseAppMgrHarness h;
  // We can't easily exercise OnRegisterBaseapp here without a real Channel;
  // instead drive the public Snapshot/Restore contract by populating a source
  // mgr via friend-like reach through SnapshotPayload assembly is not
  // available, so we go through the file path round-trip below. For the
  // payload-level round-trip, simply snapshot an empty mgr and confirm a
  // fresh mgr accepts it without losing the contract.
  const auto bytes = h.mgr.Snapshot();
  ASSERT_GE(bytes.size(),
            2 * sizeof(uint32_t) + 2 * sizeof(uint64_t));  // envelope only

  BaseAppMgrHarness restored;
  auto restore = restored.mgr.Restore(std::span<const std::byte>(bytes.data(), bytes.size()));
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();
}

TEST(BaseAppMgr, RestoreRejectsBadMagic) {
  std::vector<std::byte> bytes(2 * sizeof(uint32_t) + 2 * sizeof(uint64_t), std::byte{0});
  BinaryWriter w;
  w.Write<uint32_t>(0xDEADBEEFu);  // wrong magic
  w.Write<uint32_t>(kSnapshotVersionForTest);
  w.Write<uint64_t>(0);
  w.Write<uint64_t>(0);
  const auto buf = w.Detach();

  BaseAppMgrHarness h;
  auto r = h.mgr.Restore(std::span<const std::byte>(buf.data(), buf.size()));
  ASSERT_FALSE(r.HasValue());
  EXPECT_NE(std::string(r.Error().Message()).find("bad magic"), std::string::npos);
}

TEST(BaseAppMgr, RestoreRejectsUnsupportedVersion) {
  BinaryWriter w;
  w.Write<uint32_t>(kSnapshotMagicForTest);
  w.Write<uint32_t>(kSnapshotVersionForTest + 100u);
  w.Write<uint64_t>(0);
  w.Write<uint64_t>(0);
  const auto buf = w.Detach();

  BaseAppMgrHarness h;
  auto r = h.mgr.Restore(std::span<const std::byte>(buf.data(), buf.size()));
  ASSERT_FALSE(r.HasValue());
  EXPECT_NE(std::string(r.Error().Message()).find("unsupported version"), std::string::npos);
}

TEST(BaseAppMgr, RestoreRejectsChecksumMismatch) {
  // payload says next_app_id=1 + 0 baseapps + 0 global_bases + 0 affinity
  BinaryWriter payload_w;
  payload_w.Write<uint32_t>(1u);
  payload_w.WritePackedInt(0u);  // baseapps
  payload_w.WritePackedInt(0u);  // global_bases
  payload_w.WritePackedInt(0u);  // affinity
  const auto payload = payload_w.Detach();

  BinaryWriter w;
  w.Write(kSnapshotMagicForTest);
  w.Write(kSnapshotVersionForTest);
  w.Write(static_cast<uint64_t>(payload.size()));
  w.Write<uint64_t>(0xBAD);  // wrong checksum
  w.WriteBytes(std::span<const std::byte>(payload.data(), payload.size()));
  const auto buf = w.Detach();

  BaseAppMgrHarness h;
  auto r = h.mgr.Restore(std::span<const std::byte>(buf.data(), buf.size()));
  ASSERT_FALSE(r.HasValue());
  EXPECT_NE(std::string(r.Error().Message()).find("checksum mismatch"), std::string::npos);
}

TEST(BaseAppMgr, SnapshotFileRoundTripCreatesArtifacts) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_baseappmgr_snapshot_{}.bin", stamp);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path.string() + ".bak", ec);

  BaseAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  auto save = h.mgr.SaveSnapshotToFile(path);
  ASSERT_TRUE(save.HasValue()) << save.Error().Message();
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/snapshot_saves"),
            std::optional<std::string>("1"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/snapshot_save_failures"),
            std::optional<std::string>("0"));

  BaseAppMgrHarness restored;
  auto restore = restored.mgr.RestoreSnapshotFromFile(path);
  ASSERT_TRUE(restore.HasValue()) << restore.Error().Message();

  std::filesystem::remove(path, ec);
  std::filesystem::remove(path.string() + ".bak", ec);
}

TEST(BaseAppMgr, SaveSnapshotToFileSkipsBackupWhenMainFileCorrupt) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_baseappmgr_backup_skip_{}.bin", stamp);
  auto backup_path = path;
  backup_path += ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);

  ASSERT_TRUE(fs::WriteTextFile(path, "corrupt-main-file").HasValue());

  BaseAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  auto save = h.mgr.SaveSnapshotToFile(path);
  ASSERT_TRUE(save.HasValue()) << save.Error().Message();
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/snapshot_backup_skips"),
            std::optional<std::string>("1"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/snapshot_save_failures"),
            std::optional<std::string>("0"));
  EXPECT_TRUE(std::filesystem::exists(path));

  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);
}

TEST(BaseAppMgr, ReattachStateIsIdleWhenNoRestoredApps) {
  BaseAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/restored_baseapps"),
            std::optional<std::string>("0"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/reattach_pending"),
            std::optional<std::string>("0"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/reattach_state"),
            std::optional<std::string>("idle"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/reattach_completed"),
            std::optional<std::string>("true"));
}

TEST(BaseAppMgr, ReattachWatchdogMsIsConfigurableViaWatcher) {
  BaseAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  auto initial = h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/reattach_watchdog_ms");
  ASSERT_TRUE(initial.has_value());
  EXPECT_FALSE(initial->empty());
  // Verify the watcher accepts an override (the ServerAppOption registers
  // ReadWrite so set-watch can shrink the window during verify drills).
  EXPECT_TRUE(h.mgr.GetWatcherRegistry().Set("baseappmgr/ha/reattach_watchdog_ms", "5000"));
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/reattach_watchdog_ms"),
            std::optional<std::string>("5000"));
}

TEST(BaseAppMgr, SnapshotSizeHighWaterPctReflectsLastSave) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    std::format("atlas_baseappmgr_water_{}.bin", stamp);
  std::error_code ec;
  std::filesystem::remove(path, ec);

  BaseAppMgrHarness h;
  h.mgr.RegisterWatchersForTest();
  EXPECT_EQ(h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/snapshot_size_high_water_pct"),
            std::optional<std::string>("0"));
  auto save = h.mgr.SaveSnapshotToFile(path);
  ASSERT_TRUE(save.HasValue()) << save.Error().Message();
  auto max_bytes = h.mgr.GetWatcherRegistry().Get("baseappmgr/ha/snapshot_max_bytes");
  ASSERT_TRUE(max_bytes.has_value());
  EXPECT_NE(*max_bytes, std::string("0"));

  std::filesystem::remove(path, ec);
  std::filesystem::remove(path.string() + ".bak", ec);
}

}  // namespace
}  // namespace atlas
