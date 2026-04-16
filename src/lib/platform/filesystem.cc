#include "platform/filesystem.h"

#include <fstream>
#include <sstream>

#if ATLAS_PLATFORM_WINDOWS
#include <windows.h>
#elif ATLAS_PLATFORM_LINUX
#include <linux/limits.h>
#include <unistd.h>
#endif

namespace atlas::fs {

[[nodiscard]] auto ReadFile(const stdfs::path& path) -> Result<std::vector<std::byte>> {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return Error{ErrorCode::kIoError, "Failed to open file: " + path.string()};
  }

  file.seekg(0, std::ios::end);
  auto size = file.tellg();
  file.seekg(0, std::ios::beg);

  if (size < 0) {
    return Error{ErrorCode::kIoError, "Failed to determine file size: " + path.string()};
  }

  std::vector<std::byte> buffer(static_cast<std::size_t>(size));
  file.read(reinterpret_cast<char*>(buffer.data()), size);

  if (!file) {
    return Error{ErrorCode::kIoError, "Failed to read file: " + path.string()};
  }

  return buffer;
}

[[nodiscard]] auto ReadTextFile(const stdfs::path& path) -> Result<std::string> {
  std::ifstream file(path);
  if (!file) {
    return Error{ErrorCode::kIoError, "Failed to open file: " + path.string()};
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  if (!file && !file.eof()) {
    return Error{ErrorCode::kIoError, "Failed to read file: " + path.string()};
  }

  return ss.str();
}

[[nodiscard]] auto WriteFile(const stdfs::path& path, std::span<const std::byte> data)
    -> Result<void> {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return Error{ErrorCode::kIoError, "Failed to open file for writing: " + path.string()};
  }

  file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

  if (!file) {
    return Error{ErrorCode::kIoError, "Failed to write file: " + path.string()};
  }

  return Result<void>{};
}

[[nodiscard]] auto WriteTextFile(const stdfs::path& path, std::string_view content)
    -> Result<void> {
  std::ofstream file(path);
  if (!file) {
    return Error{ErrorCode::kIoError, "Failed to open file for writing: " + path.string()};
  }

  file.write(content.data(), static_cast<std::streamsize>(content.size()));

  if (!file) {
    return Error{ErrorCode::kIoError, "Failed to write file: " + path.string()};
  }

  return Result<void>{};
}

[[nodiscard]] auto Exists(const stdfs::path& path) -> bool {
  std::error_code ec;
  return stdfs::exists(path, ec);
}

[[nodiscard]] auto FileSize(const stdfs::path& path) -> Result<std::uintmax_t> {
  std::error_code ec;
  auto size = stdfs::file_size(path, ec);
  if (ec) {
    return Error{ErrorCode::kIoError, "Failed to get file size: " + ec.message()};
  }
  return size;
}

[[nodiscard]] auto CreateDirectories(const stdfs::path& path) -> Result<void> {
  std::error_code ec;
  stdfs::create_directories(path, ec);
  if (ec) {
    return Error{ErrorCode::kIoError, "Failed to create directories: " + ec.message()};
  }
  return Result<void>{};
}

[[nodiscard]] auto RemoveFile(const stdfs::path& path) -> Result<bool> {
  std::error_code ec;
  bool removed = stdfs::remove(path, ec);
  if (ec) {
    return Error{ErrorCode::kIoError, "Failed to remove file: " + ec.message()};
  }
  return removed;
}

[[nodiscard]] auto ListDirectory(const stdfs::path& path)
    -> Result<std::vector<stdfs::directory_entry>> {
  std::error_code ec;
  std::vector<stdfs::directory_entry> entries;

  auto it = stdfs::directory_iterator(path, ec);
  if (ec) {
    return Error{ErrorCode::kIoError, "Failed to open directory: " + ec.message()};
  }

  for (const auto& entry : it) {
    entries.push_back(entry);
  }

  return entries;
}

[[nodiscard]] auto ExecutablePath() -> Result<stdfs::path> {
#if ATLAS_PLATFORM_WINDOWS
  wchar_t buffer[MAX_PATH];
  DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    return Error{ErrorCode::kIoError, "Failed to get executable path"};
  }
  return stdfs::path{buffer};
#elif ATLAS_PLATFORM_LINUX
  char buffer[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", buffer, PATH_MAX);
  if (length < 0 || length >= PATH_MAX) {
    return Error{ErrorCode::kIoError, "Failed to get executable path"};
  }
  return stdfs::path{std::string(buffer, static_cast<std::size_t>(length))};
#else
  return Error{ErrorCode::kNotSupported, "executable_path not supported on this platform"};
#endif
}

[[nodiscard]] auto TempDirectory() -> stdfs::path {
  return stdfs::temp_directory_path();
}

}  // namespace atlas::fs
