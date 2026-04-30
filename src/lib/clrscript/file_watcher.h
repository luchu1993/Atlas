#ifndef ATLAS_LIB_CLRSCRIPT_FILE_WATCHER_H_
#define ATLAS_LIB_CLRSCRIPT_FILE_WATCHER_H_

#include <filesystem>
#include <string>
#include <unordered_map>

namespace atlas {

class FileWatcher {
 public:
  explicit FileWatcher(const std::filesystem::path& directory);

  [[nodiscard]] auto CheckChanges() -> bool;

  void Reset();

 private:
  std::filesystem::path directory_;
  std::unordered_map<std::string, std::filesystem::file_time_type> timestamps_;

  void Scan(std::unordered_map<std::string, std::filesystem::file_time_type>& out) const;

  static bool IsExcluded(const std::filesystem::path& path);
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_FILE_WATCHER_H_
