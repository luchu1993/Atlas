#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace atlas
{

/// Polling-based file watcher. Checks .cs files' last_write_time to detect changes.
/// Excludes bin/, obj/, .git/, .reload_staging/, .reload_backup/ directories.
class FileWatcher
{
public:
    explicit FileWatcher(const std::filesystem::path& directory);

    /// Scan directory and return true if any .cs file has changed since last scan.
    [[nodiscard]] auto check_changes() -> bool;

    /// Re-scan and record current timestamps without reporting changes.
    void reset();

private:
    std::filesystem::path directory_;
    std::unordered_map<std::string, std::filesystem::file_time_type> timestamps_;

    void scan(std::unordered_map<std::string, std::filesystem::file_time_type>& out) const;

    static bool is_excluded(const std::filesystem::path& path);
};

}  // namespace atlas
