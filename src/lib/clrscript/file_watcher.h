#ifndef ATLAS_LIB_CLRSCRIPT_FILE_WATCHER_H_
#define ATLAS_LIB_CLRSCRIPT_FILE_WATCHER_H_

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace atlas {

/// Polling-based file watcher. Checks .cs files' last_write_time to detect changes.
/// Excludes bin/, obj/, .git/, .reload_staging/, .reload_backup/ directories.
class FileWatcher {
 public:
  explicit FileWatcher(const std::filesystem::path& directory);

  /// Scan directory and return true if any .cs file has changed since last scan.
  [[nodiscard]] auto CheckChanges() -> bool;

  /// Re-scan and record current timestamps without reporting changes.
  void Reset();

 private:
  std::filesystem::path directory_;
  std::unordered_map<std::string, std::filesystem::file_time_type> timestamps_;

  void Scan(std::unordered_map<std::string, std::filesystem::file_time_type>& out) const;

  static bool IsExcluded(const std::filesystem::path& path);
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_FILE_WATCHER_H_
