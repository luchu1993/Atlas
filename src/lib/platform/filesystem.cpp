#include "platform/filesystem.hpp"

#include <fstream>
#include <sstream>

#if ATLAS_PLATFORM_WINDOWS
#   include <windows.h>
#elif ATLAS_PLATFORM_LINUX
#   include <unistd.h>
#   include <linux/limits.h>
#endif

namespace atlas::fs
{

[[nodiscard]] auto read_file(const stdfs::path& path) -> Result<std::vector<std::byte>>
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return Error{ErrorCode::IoError, "Failed to open file: " + path.string()};
    }

    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> buffer(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(buffer.data()), size);

    if (!file)
    {
        return Error{ErrorCode::IoError, "Failed to read file: " + path.string()};
    }

    return buffer;
}

[[nodiscard]] auto read_text_file(const stdfs::path& path) -> Result<std::string>
{
    std::ifstream file(path);
    if (!file)
    {
        return Error{ErrorCode::IoError, "Failed to open file: " + path.string()};
    }

    std::ostringstream ss;
    ss << file.rdbuf();

    if (!file && !file.eof())
    {
        return Error{ErrorCode::IoError, "Failed to read file: " + path.string()};
    }

    return ss.str();
}

[[nodiscard]] auto write_file(const stdfs::path& path, std::span<const std::byte> data) -> Result<void>
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        return Error{ErrorCode::IoError, "Failed to open file for writing: " + path.string()};
    }

    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

    if (!file)
    {
        return Error{ErrorCode::IoError, "Failed to write file: " + path.string()};
    }

    return Result<void>{};
}

[[nodiscard]] auto write_text_file(const stdfs::path& path, std::string_view content) -> Result<void>
{
    std::ofstream file(path);
    if (!file)
    {
        return Error{ErrorCode::IoError, "Failed to open file for writing: " + path.string()};
    }

    file.write(content.data(), static_cast<std::streamsize>(content.size()));

    if (!file)
    {
        return Error{ErrorCode::IoError, "Failed to write file: " + path.string()};
    }

    return Result<void>{};
}

[[nodiscard]] auto exists(const stdfs::path& path) -> bool
{
    std::error_code ec;
    return stdfs::exists(path, ec);
}

[[nodiscard]] auto file_size(const stdfs::path& path) -> Result<std::uintmax_t>
{
    std::error_code ec;
    auto size = stdfs::file_size(path, ec);
    if (ec)
    {
        return Error{ErrorCode::IoError, "Failed to get file size: " + ec.message()};
    }
    return size;
}

[[nodiscard]] auto create_directories(const stdfs::path& path) -> Result<void>
{
    std::error_code ec;
    stdfs::create_directories(path, ec);
    if (ec)
    {
        return Error{ErrorCode::IoError, "Failed to create directories: " + ec.message()};
    }
    return Result<void>{};
}

[[nodiscard]] auto remove_file(const stdfs::path& path) -> Result<bool>
{
    std::error_code ec;
    bool removed = stdfs::remove(path, ec);
    if (ec)
    {
        return Error{ErrorCode::IoError, "Failed to remove file: " + ec.message()};
    }
    return removed;
}

[[nodiscard]] auto list_directory(const stdfs::path& path) -> Result<std::vector<stdfs::directory_entry>>
{
    std::error_code ec;
    std::vector<stdfs::directory_entry> entries;

    auto it = stdfs::directory_iterator(path, ec);
    if (ec)
    {
        return Error{ErrorCode::IoError, "Failed to open directory: " + ec.message()};
    }

    for (const auto& entry : it)
    {
        entries.push_back(entry);
    }

    return entries;
}

[[nodiscard]] auto executable_path() -> Result<stdfs::path>
{
#if ATLAS_PLATFORM_WINDOWS
    wchar_t buffer[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
    {
        return Error{ErrorCode::IoError, "Failed to get executable path"};
    }
    return stdfs::path{buffer};
#elif ATLAS_PLATFORM_LINUX
    char buffer[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", buffer, PATH_MAX);
    if (length < 0 || length >= PATH_MAX)
    {
        return Error{ErrorCode::IoError, "Failed to get executable path"};
    }
    return stdfs::path{std::string(buffer, static_cast<std::size_t>(length))};
#else
    return Error{ErrorCode::NotSupported, "executable_path not supported on this platform"};
#endif
}

[[nodiscard]] auto temp_directory() -> stdfs::path
{
    return stdfs::temp_directory_path();
}

} // namespace atlas::fs
