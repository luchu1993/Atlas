#include "clrscript/file_watcher.h"

#include <algorithm>

namespace atlas {

FileWatcher::FileWatcher(const std::filesystem::path& directory) : directory_(directory) {
  Scan(timestamps_);
}

auto FileWatcher::CheckChanges() -> bool {
  std::unordered_map<std::string, std::filesystem::file_time_type> current;
  Scan(current);

  bool changed = false;

  // Check for new or modified files
  for (const auto& [path, time] : current) {
    auto it = timestamps_.find(path);
    if (it == timestamps_.end() || it->second != time) {
      changed = true;
      break;
    }
  }

  // Check for deleted files
  if (!changed) {
    for (const auto& [path, time] : timestamps_) {
      if (current.find(path) == current.end()) {
        changed = true;
        break;
      }
    }
  }

  timestamps_ = std::move(current);
  return changed;
}

void FileWatcher::Reset() {
  timestamps_.clear();
  Scan(timestamps_);
}

void FileWatcher::Scan(
    std::unordered_map<std::string, std::filesystem::file_time_type>& out) const {
  if (!std::filesystem::exists(directory_)) return;

  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory_, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".cs") continue;
    if (IsExcluded(entry.path())) continue;

    auto write_time = entry.last_write_time(ec);
    if (!ec) out[entry.path().string()] = write_time;
  }
}

bool FileWatcher::IsExcluded(const std::filesystem::path& path) {
  auto str = path.generic_string();
  static const char* excludes[] = {"/bin/", "/obj/", "/.git/", "/.reload_staging/",
                                   "/.reload_backup/"};
  return std::ranges::any_of(
      excludes, [&str](const char* exc) { return str.find(exc) != std::string::npos; });
}

}  // namespace atlas
