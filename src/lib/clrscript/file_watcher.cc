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
  std::error_code ec;
  if (!std::filesystem::exists(directory_, ec) || ec) return;

  std::filesystem::recursive_directory_iterator it(
      directory_, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) return;

  // ec-overloads throughout: watched dirs may contain unstattable entries
  // (pipes, sockets) that would throw from the no-arg status() calls.
  for (const std::filesystem::recursive_directory_iterator end; it != end; it.increment(ec)) {
    if (ec) break;
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec) || entry_ec) continue;
    const auto& path = it->path();
    if (path.extension() != ".cs") continue;
    if (IsExcluded(path)) continue;
    auto write_time = it->last_write_time(entry_ec);
    if (!entry_ec) out[path.string()] = write_time;
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
