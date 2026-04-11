#include "clrscript/file_watcher.hpp"

namespace atlas
{

FileWatcher::FileWatcher(const std::filesystem::path& directory) : directory_(directory)
{
    scan(timestamps_);
}

auto FileWatcher::check_changes() -> bool
{
    std::unordered_map<std::string, std::filesystem::file_time_type> current;
    scan(current);

    bool changed = false;

    // Check for new or modified files
    for (const auto& [path, time] : current)
    {
        auto it = timestamps_.find(path);
        if (it == timestamps_.end() || it->second != time)
        {
            changed = true;
            break;
        }
    }

    // Check for deleted files
    if (!changed)
    {
        for (const auto& [path, time] : timestamps_)
        {
            if (current.find(path) == current.end())
            {
                changed = true;
                break;
            }
        }
    }

    timestamps_ = std::move(current);
    return changed;
}

void FileWatcher::reset()
{
    timestamps_.clear();
    scan(timestamps_);
}

void FileWatcher::scan(std::unordered_map<std::string, std::filesystem::file_time_type>& out) const
{
    if (!std::filesystem::exists(directory_))
        return;

    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory_, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() != ".cs")
            continue;
        if (is_excluded(entry.path()))
            continue;

        auto write_time = entry.last_write_time(ec);
        if (!ec)
            out[entry.path().string()] = write_time;
    }
}

bool FileWatcher::is_excluded(const std::filesystem::path& path)
{
    auto str = path.generic_string();
    static const char* excludes[] = {"/bin/", "/obj/", "/.git/", "/.reload_staging/",
                                     "/.reload_backup/"};
    for (const auto* exc : excludes)
    {
        if (str.find(exc) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace atlas
