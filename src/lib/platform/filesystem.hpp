#pragma once

#include "foundation/error.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace atlas::fs
{

namespace stdfs = std::filesystem;

[[nodiscard]] auto read_file(const stdfs::path& path) -> Result<std::vector<std::byte>>;
[[nodiscard]] auto read_text_file(const stdfs::path& path) -> Result<std::string>;
[[nodiscard]] auto write_file(const stdfs::path& path, std::span<const std::byte> data)
    -> Result<void>;
[[nodiscard]] auto write_text_file(const stdfs::path& path, std::string_view content)
    -> Result<void>;

[[nodiscard]] auto exists(const stdfs::path& path) -> bool;
[[nodiscard]] auto file_size(const stdfs::path& path) -> Result<std::uintmax_t>;
[[nodiscard]] auto create_directories(const stdfs::path& path) -> Result<void>;
[[nodiscard]] auto remove_file(const stdfs::path& path) -> Result<bool>;
[[nodiscard]] auto list_directory(const stdfs::path& path)
    -> Result<std::vector<stdfs::directory_entry>>;

// Platform-specific
[[nodiscard]] auto executable_path() -> Result<stdfs::path>;
[[nodiscard]] auto temp_directory() -> stdfs::path;

}  // namespace atlas::fs
