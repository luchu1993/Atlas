#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "platform/filesystem.h"
#include "serialization/binary_stream.h"
#include "server/snapshot_envelope.h"

namespace atlas {
namespace {

constexpr uint32_t kTestMagic = 0x54535441u;  // 'ATST'
constexpr uint32_t kTestVersion = 7;
constexpr uint64_t kTestMaxPayload = 64ull * 1024ull;
constexpr uint64_t kTestMaxFile = kTestMaxPayload + snapshot_envelope::kEnvelopeBytes;
constexpr std::string_view kTestModule = "TestMgr";

auto MakePayload(std::initializer_list<int> ints) -> std::vector<std::byte> {
  BinaryWriter w;
  for (int v : ints) w.Write<int32_t>(v);
  return w.Detach();
}

auto TempFilePath() -> std::filesystem::path {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         std::format("atlas_snapshot_envelope_{}.bin", stamp);
}

TEST(SnapshotEnvelope, ChecksumIsDeterministic) {
  const auto payload = MakePayload({1, 2, 3, 4});
  const auto first = snapshot_envelope::Checksum(
      std::span<const std::byte>(payload.data(), payload.size()));
  const auto second = snapshot_envelope::Checksum(
      std::span<const std::byte>(payload.data(), payload.size()));
  EXPECT_EQ(first, second);
}

TEST(SnapshotEnvelope, WrapAndReadRoundtripsPayload) {
  const auto payload = MakePayload({10, 20, 30});
  const auto wrapped = snapshot_envelope::WrapPayload(
      std::span<const std::byte>(payload.data(), payload.size()), kTestMagic, kTestVersion);

  auto view = snapshot_envelope::ReadPayload(
      std::span<const std::byte>(wrapped.data(), wrapped.size()), kTestMagic, kTestVersion,
      kTestMaxPayload, kTestModule);
  ASSERT_TRUE(view.HasValue()) << view.Error().Message();
  EXPECT_EQ(view->payload.size(), payload.size());
}

TEST(SnapshotEnvelope, RejectsBadMagic) {
  const auto payload = MakePayload({1});
  const auto wrapped = snapshot_envelope::WrapPayload(
      std::span<const std::byte>(payload.data(), payload.size()), kTestMagic, kTestVersion);
  auto view = snapshot_envelope::ReadPayload(
      std::span<const std::byte>(wrapped.data(), wrapped.size()), kTestMagic + 1, kTestVersion,
      kTestMaxPayload, kTestModule);
  ASSERT_FALSE(view.HasValue());
  EXPECT_NE(std::string(view.Error().Message()).find("bad magic"), std::string::npos);
}

TEST(SnapshotEnvelope, RejectsUnsupportedVersion) {
  const auto payload = MakePayload({1});
  const auto wrapped = snapshot_envelope::WrapPayload(
      std::span<const std::byte>(payload.data(), payload.size()), kTestMagic, kTestVersion);
  auto view = snapshot_envelope::ReadPayload(
      std::span<const std::byte>(wrapped.data(), wrapped.size()), kTestMagic, kTestVersion + 1,
      kTestMaxPayload, kTestModule);
  ASSERT_FALSE(view.HasValue());
  EXPECT_NE(std::string(view.Error().Message()).find("unsupported version"), std::string::npos);
}

TEST(SnapshotEnvelope, RejectsChecksumMismatch) {
  const auto payload = MakePayload({1, 2, 3});
  auto wrapped = snapshot_envelope::WrapPayload(
      std::span<const std::byte>(payload.data(), payload.size()), kTestMagic, kTestVersion);
  // Flip one payload byte after the envelope header so the checksum mismatches.
  wrapped[snapshot_envelope::kEnvelopeBytes] ^= std::byte{0xFFu};
  auto view = snapshot_envelope::ReadPayload(
      std::span<const std::byte>(wrapped.data(), wrapped.size()), kTestMagic, kTestVersion,
      kTestMaxPayload, kTestModule);
  ASSERT_FALSE(view.HasValue());
  EXPECT_NE(std::string(view.Error().Message()).find("checksum mismatch"), std::string::npos);
}

TEST(SnapshotEnvelope, RejectsPayloadTooLarge) {
  // Build a header that claims a huge payload.
  BinaryWriter w;
  w.Write(kTestMagic);
  w.Write(kTestVersion);
  w.Write<uint64_t>(kTestMaxPayload + 1);
  w.Write<uint64_t>(0);
  const auto wrapped = w.Detach();
  auto view = snapshot_envelope::ReadPayload(
      std::span<const std::byte>(wrapped.data(), wrapped.size()), kTestMagic, kTestVersion,
      kTestMaxPayload, kTestModule);
  ASSERT_FALSE(view.HasValue());
  EXPECT_NE(std::string(view.Error().Message()).find("payload too large"), std::string::npos);
}

TEST(SnapshotEnvelope, ErrorMessageNamesModule) {
  BinaryWriter w;
  w.Write<uint32_t>(0xDEADBEEFu);
  w.Write(kTestVersion);
  w.Write<uint64_t>(0);
  w.Write<uint64_t>(0);
  const auto bad = w.Detach();
  auto view = snapshot_envelope::ReadPayload(
      std::span<const std::byte>(bad.data(), bad.size()), kTestMagic, kTestVersion,
      kTestMaxPayload, kTestModule);
  ASSERT_FALSE(view.HasValue());
  EXPECT_NE(std::string(view.Error().Message()).find("TestMgr"), std::string::npos);
}

TEST(SnapshotEnvelope, WatcherErrorDetailSanitisesAndBounds) {
  const auto detail = snapshot_envelope::WatcherErrorDetail("disk full: /tmp/atlas-snap_v1.bak");
  // Letters, digits, dot/dash/underscore preserved; spaces and colons turn
  // into underscores; collapses runs.
  EXPECT_EQ(detail, "disk_full_tmp_atlas-snap_v1.bak");
}

TEST(SnapshotEnvelope, WatcherErrorDetailHandlesEmpty) {
  EXPECT_EQ(snapshot_envelope::WatcherErrorDetail(""), "unknown");
}

TEST(SnapshotEnvelope, ReadinessReportsMissingFile) {
  const auto readiness = snapshot_envelope::Readiness(
      TempFilePath(), true, kTestMagic, kTestVersion, kTestMaxFile, kTestMaxPayload, kTestModule);
  EXPECT_FALSE(readiness.present);
  EXPECT_STREQ(readiness.state, "missing");
}

TEST(SnapshotEnvelope, ReadinessReportsReadyForGoodFile) {
  const auto payload = MakePayload({1, 2});
  const auto wrapped = snapshot_envelope::WrapPayload(
      std::span<const std::byte>(payload.data(), payload.size()), kTestMagic, kTestVersion);
  const auto path = TempFilePath();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  ASSERT_TRUE(fs::WriteFile(path, std::span<const std::byte>(wrapped.data(), wrapped.size()))
                  .HasValue());

  const auto readiness = snapshot_envelope::Readiness(
      path, true, kTestMagic, kTestVersion, kTestMaxFile, kTestMaxPayload, kTestModule);
  EXPECT_TRUE(readiness.present);
  EXPECT_TRUE(readiness.valid);
  EXPECT_FALSE(readiness.error_present);
  EXPECT_STREQ(readiness.state, "ready");

  std::filesystem::remove(path, ec);
}

TEST(SnapshotEnvelope, ReadinessFlagsCorruptFile) {
  const auto path = TempFilePath();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  ASSERT_TRUE(fs::WriteTextFile(path, "garbage-content").HasValue());

  const auto readiness = snapshot_envelope::Readiness(
      path, true, kTestMagic, kTestVersion, kTestMaxFile, kTestMaxPayload, kTestModule);
  EXPECT_TRUE(readiness.present);
  EXPECT_FALSE(readiness.valid);
  EXPECT_TRUE(readiness.error_present);
  EXPECT_STREQ(readiness.state, "invalid");
  EXPECT_NE(readiness.error_detail.find("TestMgr"), std::string::npos);

  std::filesystem::remove(path, ec);
}

TEST(SnapshotEnvelope, PreserveBackupCopiesValidMain) {
  const auto path = TempFilePath();
  const auto backup_path = path.string() + ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);

  const auto payload = MakePayload({42});
  const auto wrapped = snapshot_envelope::WrapPayload(
      std::span<const std::byte>(payload.data(), payload.size()), kTestMagic, kTestVersion);
  ASSERT_TRUE(fs::WriteFile(path, std::span<const std::byte>(wrapped.data(), wrapped.size()))
                  .HasValue());

  auto preserve = snapshot_envelope::PreserveBackup(path, kTestMagic, kTestVersion,
                                                    kTestMaxPayload, kTestModule);
  ASSERT_TRUE(preserve.HasValue()) << preserve.Error().Message();
  EXPECT_TRUE(std::filesystem::exists(backup_path));

  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);
}

TEST(SnapshotEnvelope, PreserveBackupRefusesCorruptMain) {
  const auto path = TempFilePath();
  const auto backup_path = path.string() + ".bak";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(backup_path, ec);

  ASSERT_TRUE(fs::WriteTextFile(path, "corrupt").HasValue());
  auto preserve = snapshot_envelope::PreserveBackup(path, kTestMagic, kTestVersion,
                                                    kTestMaxPayload, kTestModule);
  ASSERT_FALSE(preserve.HasValue());
  EXPECT_EQ(preserve.Error().Code(), ErrorCode::kInvalidArgument);
  // Caller is expected to log + bump backup_skip_count; we just confirm the
  // existing backup wasn't created from corrupt bytes.
  EXPECT_FALSE(std::filesystem::exists(backup_path));

  std::filesystem::remove(path, ec);
}

TEST(SnapshotEnvelope, EvaluateSizeWarningBelowThresholdReturnsNothing) {
  const auto now = Clock::now();
  // Below 80% with no prior warning: no log, no reset.
  const auto d = snapshot_envelope::EvaluateSizeWarning(75u, now, TimePoint{});
  EXPECT_FALSE(d.should_log);
  EXPECT_FALSE(d.should_reset);
}

TEST(SnapshotEnvelope, EvaluateSizeWarningBelowThresholdResetsPriorTimestamp) {
  const auto now = Clock::now();
  const auto last = now - std::chrono::seconds(10);
  // Snapshot pressure relieved after a prior warning — caller should clear
  // last_warned_at so a future spike re-warns immediately.
  const auto d = snapshot_envelope::EvaluateSizeWarning(50u, now, last);
  EXPECT_FALSE(d.should_log);
  EXPECT_TRUE(d.should_reset);
}

TEST(SnapshotEnvelope, EvaluateSizeWarningAtThresholdLogsFirstTime) {
  const auto now = Clock::now();
  const auto d = snapshot_envelope::EvaluateSizeWarning(80u, now, TimePoint{});
  EXPECT_TRUE(d.should_log);
  EXPECT_FALSE(d.should_reset);
}

TEST(SnapshotEnvelope, EvaluateSizeWarningJustBelowThresholdSilent) {
  const auto now = Clock::now();
  // 79% must not trip the warning — threshold is inclusive at 80.
  const auto d = snapshot_envelope::EvaluateSizeWarning(79u, now, TimePoint{});
  EXPECT_FALSE(d.should_log);
  EXPECT_FALSE(d.should_reset);
}

TEST(SnapshotEnvelope, EvaluateSizeWarningThrottlesRepeatLogs) {
  const auto now = Clock::now();
  const auto recent = now - std::chrono::seconds(30);  // < 60s throttle
  const auto d = snapshot_envelope::EvaluateSizeWarning(90u, now, recent);
  EXPECT_FALSE(d.should_log);
  EXPECT_FALSE(d.should_reset);
}

TEST(SnapshotEnvelope, EvaluateSizeWarningReissuesAfterThrottleWindow) {
  const auto now = Clock::now();
  const auto stale = now - std::chrono::seconds(120);  // > 60s throttle
  const auto d = snapshot_envelope::EvaluateSizeWarning(95u, now, stale);
  EXPECT_TRUE(d.should_log);
  EXPECT_FALSE(d.should_reset);
}

TEST(SnapshotEnvelope, EvaluateSizeWarningExactlyAtThrottleBoundary) {
  const auto now = Clock::now();
  const auto exactly_throttle_ago = now - std::chrono::duration_cast<Duration>(
                                              snapshot_envelope::kSizeWarningThrottle);
  // now - last_warned_at == throttle should re-warn (the comparison is >=).
  const auto d = snapshot_envelope::EvaluateSizeWarning(85u, now, exactly_throttle_ago);
  EXPECT_TRUE(d.should_log);
  EXPECT_FALSE(d.should_reset);
}

TEST(SnapshotEnvelope, EvaluateSizeWarningCustomThreshold) {
  const auto now = Clock::now();
  // Lower threshold to 50% — 60% should now trip.
  const auto d = snapshot_envelope::EvaluateSizeWarning(60u, now, TimePoint{}, 50u);
  EXPECT_TRUE(d.should_log);
}

}  // namespace
}  // namespace atlas
