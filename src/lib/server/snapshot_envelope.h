#ifndef ATLAS_LIB_SERVER_SNAPSHOT_ENVELOPE_H_
#define ATLAS_LIB_SERVER_SNAPSHOT_ENVELOPE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/error.h"
#include "foundation/clock.h"
#include "platform/filesystem.h"
#include "serialization/binary_stream.h"

namespace atlas::snapshot_envelope {

inline constexpr uint64_t kChecksumSeed = 14695981039346656037ull;
inline constexpr uint64_t kChecksumPrime = 1099511628211ull;
inline constexpr uint64_t kEnvelopeBytes = 2ull * sizeof(uint32_t) + 2ull * sizeof(uint64_t);
inline constexpr std::size_t kMaxWatcherErrorDetailBytes = 120;
inline constexpr uint32_t kSizeWarningThresholdPct = 80;
inline constexpr auto kSizeWarningThrottle = std::chrono::seconds(60);

struct PayloadView {
  std::span<const std::byte> payload;
};

// Decision returned by EvaluateSizeWarning. Both flags can be false
// (in which case caller does nothing); they're never both true at once.
struct SizeWarningDecision {
  bool should_log{false};    // pct ≥ threshold and throttle satisfied
  bool should_reset{false};  // pct dropped back below threshold; clear last_warned_at
};

// Pure decision about whether SaveSnapshotToFile should log a high-water
// WARNING. Callers feed in the current size percentage (0..100 from
// SnapshotSizeHighWaterPct), the current time, the throttle's
// last_warned_at TimePoint, and (optionally) overridden threshold /
// throttle. Returning a decision struct keeps the side-effect (logging
// + last_warned_at update) at the call site so this helper stays unit-
// testable without a mock Clock.
inline auto EvaluateSizeWarning(uint32_t pct, TimePoint now, TimePoint last_warned_at,
                                uint32_t threshold_pct = kSizeWarningThresholdPct,
                                Duration throttle = std::chrono::duration_cast<Duration>(
                                    kSizeWarningThrottle)) -> SizeWarningDecision {
  if (pct < threshold_pct) {
    // Snapshot dropped back below the high-water mark; clear the throttle
    // so a future spike re-warns immediately instead of waiting out the
    // previous warning's window.
    return {false, last_warned_at != TimePoint{}};
  }
  if (last_warned_at == TimePoint{} || now - last_warned_at >= throttle) {
    return {true, false};
  }
  return {false, false};
}

struct FileReadiness {
  bool present{false};
  uint64_t bytes{0};
  bool valid{false};
  bool error_present{false};
  const char* state{"missing"};
  std::string error_detail{"none"};
};

inline auto Checksum(std::span<const std::byte> bytes) -> uint64_t {
  uint64_t hash = kChecksumSeed;
  for (std::byte byte : bytes) {
    hash ^= std::to_integer<uint8_t>(byte);
    hash *= kChecksumPrime;
  }
  return hash;
}

// Format a watcher error_detail token: ASCII alphanumerics, dot, dash,
// underscore; runs of other characters collapse into a single underscore.
// Bounded to kMaxWatcherErrorDetailBytes so a long std error message never
// blows up a single watcher line.
inline auto WatcherErrorDetail(std::string_view message) -> std::string {
  if (message.empty()) return "unknown";
  std::string out;
  out.reserve(std::min(message.size(), kMaxWatcherErrorDetailBytes));
  bool pending_separator = false;
  for (const char ch : message) {
    const bool keep = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
    if (keep) {
      if (pending_separator && !out.empty() && out.size() < kMaxWatcherErrorDetailBytes) {
        out.push_back('_');
      }
      pending_separator = false;
      if (out.size() == kMaxWatcherErrorDetailBytes) break;
      out.push_back(ch);
      continue;
    }
    pending_separator = !out.empty();
  }
  if (!out.empty() && out.back() == '_') out.pop_back();
  if (out.empty()) return "unknown";
  return out;
}

inline auto BackupPath(const std::filesystem::path& path) -> std::filesystem::path {
  auto backup = path;
  backup += ".bak";
  return backup;
}

inline auto TempPath(const std::filesystem::path& path) -> std::filesystem::path {
  auto tmp = path;
  tmp += ".tmp";
  return tmp;
}

// Wrap a snapshot payload with magic/version envelope + checksum.
inline auto WrapPayload(std::span<const std::byte> payload, uint32_t magic, uint32_t version)
    -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write(magic);
  w.Write(version);
  w.Write(static_cast<uint64_t>(payload.size()));
  w.Write(Checksum(payload));
  w.WriteBytes(payload);
  return w.Detach();
}

// Read a snapshot blob and validate magic/version/size/checksum. Returns
// a PayloadView whose `payload` borrows from the caller-owned input bytes.
// `module_name` participates in error messages so log/watcher output can
// say "CellAppMgr snapshot: bad magic" vs "BaseAppMgr snapshot: bad magic".
inline auto ReadPayload(std::span<const std::byte> bytes, uint32_t expected_magic,
                        uint32_t expected_version, uint64_t max_payload_bytes,
                        std::string_view module_name) -> Result<PayloadView> {
  BinaryReader header(bytes);
  auto magic = header.Read<uint32_t>();
  auto version = header.Read<uint32_t>();
  if (!magic || !version) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("{} snapshot: header truncated", module_name)};
  }
  if (*magic != expected_magic) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("{} snapshot: bad magic", module_name)};
  }
  if (*version != expected_version) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("{} snapshot: unsupported version", module_name)};
  }
  auto payload_size = header.Read<uint64_t>();
  auto checksum = header.Read<uint64_t>();
  if (!payload_size || !checksum) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("{} snapshot: envelope truncated", module_name)};
  }
  if (*payload_size > max_payload_bytes) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("{} snapshot: payload too large", module_name)};
  }
  auto payload = header.ReadBytes(static_cast<std::size_t>(*payload_size));
  if (!payload) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("{} snapshot: payload truncated", module_name)};
  }
  if (header.Remaining() != 0) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("{} snapshot: trailing bytes", module_name)};
  }
  if (Checksum(*payload) != *checksum) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("{} snapshot: checksum mismatch", module_name)};
  }
  return PayloadView{*payload};
}

// Compute the readiness of a snapshot file on disk:
//   - validate=false: only check presence/size; `valid` reflects "size in
//     range" rather than envelope correctness. Cheap, used in hot watcher.
//   - validate=true: additionally read the file and run ReadPayload on it
//     so the watcher reports envelope corruption. Used in the dry-run
//     readiness watchers and verify scripts.
inline auto Readiness(const std::filesystem::path& path, bool validate, uint32_t expected_magic,
                      uint32_t expected_version, uint64_t max_file_bytes,
                      uint64_t max_payload_bytes, std::string_view module_name)
    -> FileReadiness {
  if (path.empty()) return FileReadiness{false, 0, false, false, "disabled", "none"};
  if (!fs::Exists(path)) return FileReadiness{false, 0, false, false, "missing", "none"};
  auto size = fs::FileSize(path);
  if (!size) {
    return FileReadiness{true, 0, false, true, "unreadable",
                         WatcherErrorDetail(size.Error().Message())};
  }
  const auto bytes_size = static_cast<uint64_t>(*size);
  if (bytes_size == 0) return FileReadiness{true, 0, false, false, "empty", "none"};
  if (*size > max_file_bytes) {
    return FileReadiness{true, bytes_size, false, true, "too_large", "file_too_large"};
  }
  if (!validate) return FileReadiness{true, bytes_size, true, false, "ready", "none"};
  auto file = fs::ReadFile(path);
  if (!file) {
    return FileReadiness{true, bytes_size, false, true, "unreadable",
                         WatcherErrorDetail(file.Error().Message())};
  }
  auto payload = ReadPayload(std::span<const std::byte>(file->data(), file->size()),
                             expected_magic, expected_version, max_payload_bytes, module_name);
  if (!payload) {
    return FileReadiness{true, bytes_size, false, true, "invalid",
                         WatcherErrorDetail(payload.Error().Message())};
  }
  return FileReadiness{true, bytes_size, true, false, "ready", "none"};
}

// Atomically copy the current main file into <path>.bak, refusing to
// overwrite a valid backup with a corrupt main file. Returns
// kInvalidArgument when the current main file fails envelope validation —
// callers should treat that as a "leave backup alone" signal (typically
// log + skip count) rather than a hard error.
inline auto PreserveBackup(const std::filesystem::path& path, uint32_t expected_magic,
                           uint32_t expected_version, uint64_t max_payload_bytes,
                           std::string_view module_name) -> Result<void> {
  if (!fs::Exists(path)) return {};
  auto current = fs::ReadFile(path);
  if (!current) return current.Error();
  auto payload = ReadPayload(std::span<const std::byte>(current->data(), current->size()),
                             expected_magic, expected_version, max_payload_bytes, module_name);
  if (!payload) return payload.Error();

  const auto backup = BackupPath(path);
  auto tmp = TempPath(backup);
  auto write = fs::WriteFile(tmp, std::span<const std::byte>(current->data(), current->size()));
  if (!write) {
    (void)fs::RemoveFile(tmp);
    return write.Error();
  }
  auto replace = fs::AtomicReplaceFile(tmp, backup);
  if (!replace) {
    (void)fs::RemoveFile(tmp);
    return replace.Error();
  }
  return {};
}

}  // namespace atlas::snapshot_envelope

#endif  // ATLAS_LIB_SERVER_SNAPSHOT_ENVELOPE_H_
