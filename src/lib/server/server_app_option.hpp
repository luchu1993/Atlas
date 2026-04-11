#pragma once

#include "serialization/data_section.hpp"
#include "server/server_config.hpp"
#include "server/watcher.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

// ============================================================================
// ServerAppOption<T>
//
// Declare a static instance in a .cpp file to automatically:
//   1. Load the value from the JSON config (via ServerConfig::raw_config).
//   2. Register a Watcher entry when ServerApp::register_watchers() is called.
//
// Example usage in baseapp.cpp:
//
//   static ServerAppOption<int> s_backup_period{
//       10, "backup_period", "baseapp/backup_period", WatcherMode::ReadWrite};
//
// The static instances register themselves in a process-global list at
// construction time. ServerConfig::apply_options() and
// ServerApp::register_watchers() iterate the list.
// ============================================================================

template <typename T>
class ServerAppOption
{
public:
    ServerAppOption(T default_value, std::string_view json_key, std::string_view watcher_path,
                    WatcherMode mode = WatcherMode::ReadOnly)
        : value_(default_value),
          default_(default_value),
          json_key_(json_key),
          watcher_path_(watcher_path),
          mode_(mode)
    {
        all_options().push_back(this);
    }

    ~ServerAppOption()
    {
        auto& opts = all_options();
        opts.erase(std::remove(opts.begin(), opts.end(), this), opts.end());
    }

    // Non-copyable, non-movable (stored by pointer in global list)
    ServerAppOption(const ServerAppOption&) = delete;
    ServerAppOption& operator=(const ServerAppOption&) = delete;

    [[nodiscard]] auto value() const -> const T& { return value_; }

    void set_value(const T& v)
    {
        if (mode_ == WatcherMode::ReadWrite)
            value_ = v;
    }

    /// Load value from the config's raw DataSection tree.
    /// Called by ServerConfig::apply_options() after JSON loading.
    void load_from(const DataSection& root)
    {
        if constexpr (std::is_same_v<T, bool>)
            value_ = root.read_bool(json_key_, default_);
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
            value_ = static_cast<T>(root.read_int(json_key_, static_cast<int32_t>(default_)));
        else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
            value_ = static_cast<T>(root.read_uint(json_key_, static_cast<uint32_t>(default_)));
        else if constexpr (std::is_floating_point_v<T>)
            value_ = static_cast<T>(root.read_float(json_key_, static_cast<float>(default_)));
        else
            value_ = static_cast<T>(root.read_string(json_key_, ""));
    }

    /// Register this option's value as a Watcher entry.
    void register_watcher(WatcherRegistry& registry)
    {
        if (mode_ == WatcherMode::ReadWrite)
            registry.add_rw(watcher_path_, value_);
        else
            registry.add(watcher_path_, value_);
    }

    // ---- Global registry ---------------------------------------------------

    /// Process-wide list of all ServerAppOption instances.
    /// Used by ServerConfig::apply_options() and ServerApp::register_watchers().
    static auto all_options() -> std::vector<ServerAppOption*>&
    {
        static std::vector<ServerAppOption*> s_options;
        return s_options;
    }

    /// Apply all registered options from the given DataSection root.
    static void apply_all(const DataSection& root)
    {
        for (auto* opt : all_options())
            opt->load_from(root);
    }

    /// Register all options into the given WatcherRegistry.
    static void register_all(WatcherRegistry& registry)
    {
        for (auto* opt : all_options())
            opt->register_watcher(registry);
    }

private:
    T value_;
    T default_;
    std::string json_key_;
    std::string watcher_path_;
    WatcherMode mode_;
};

}  // namespace atlas
