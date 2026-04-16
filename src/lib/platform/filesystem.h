#ifndef ATLAS_LIB_PLATFORM_FILESYSTEM_H_
#define ATLAS_LIB_PLATFORM_FILESYSTEM_H_

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "foundation/error.h"

namespace atlas::fs {

namespace stdfs = std::filesystem;

[[nodiscard]] auto ReadFile(const stdfs::path& path) -> Result<std::vector<std::byte>>;
[[nodiscard]] auto ReadTextFile(const stdfs::path& path) -> Result<std::string>;
[[nodiscard]] auto WriteFile(const stdfs::path& path, std::span<const std::byte> data)
    -> Result<void>;
[[nodiscard]] auto WriteTextFile(const stdfs::path& path, std::string_view content) -> Result<void>;

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
