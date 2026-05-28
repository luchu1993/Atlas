#ifndef ATLAS_LIB_PLATFORM_FILESYSTEM_H_
#define ATLAS_LIB_PLATFORM_FILESYSTEM_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "foundation/error.h"

namespace atlas::fs {

namespace stdfs = std::filesystem;

class ScopedFileLock {
 public:
  ScopedFileLock();
  ~ScopedFileLock();

  ScopedFileLock(const ScopedFileLock&) = delete;
  auto operator=(const ScopedFileLock&) -> ScopedFileLock& = delete;
  ScopedFileLock(ScopedFileLock&& other) noexcept;
  auto operator=(ScopedFileLock&& other) noexcept -> ScopedFileLock&;

  [[nodiscard]] static auto TryAcquire(const stdfs::path& path, std::string_view content = {})
      -> Result<ScopedFileLock>;
  [[nodiscard]] auto IsHeld() const -> bool;

 private:
  struct Impl;
  explicit ScopedFileLock(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] auto ReadFile(const stdfs::path& path) -> Result<std::vector<std::byte>>;
[[nodiscard]] auto ReadTextFile(const stdfs::path& path) -> Result<std::string>;
[[nodiscard]] auto WriteFile(const stdfs::path& path, std::span<const std::byte> data)
    -> Result<void>;
[[nodiscard]] auto WriteTextFile(const stdfs::path& path, std::string_view content) -> Result<void>;
[[nodiscard]] auto AtomicReplaceFile(const stdfs::path& source, const stdfs::path& target)
    -> Result<void>;

[[nodiscard]] auto Exists(const stdfs::path& path) -> bool;
[[nodiscard]] auto FileSize(const stdfs::path& path) -> Result<std::uintmax_t>;
[[nodiscard]] auto CreateDirectories(const stdfs::path& path) -> Result<void>;
[[nodiscard]] auto RemoveFile(const stdfs::path& path) -> Result<bool>;
[[nodiscard]] auto ListDirectory(const stdfs::path& path)
    -> Result<std::vector<stdfs::directory_entry>>;

// Platform-specific
[[nodiscard]] auto ExecutablePath() -> Result<stdfs::path>;
[[nodiscard]] auto TempDirectory() -> stdfs::path;

}  // namespace atlas::fs

#endif  // ATLAS_LIB_PLATFORM_FILESYSTEM_H_
