#ifndef ATLAS_LIB_SERVER_SERVER_APP_OPTION_H_
#define ATLAS_LIB_SERVER_SERVER_APP_OPTION_H_

#include <string>
#include <string_view>
#include <vector>

#include "serialization/data_section.h"
#include "server/server_config.h"
#include "server/watcher.h"

namespace atlas {

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
class ServerAppOption {
 public:
  ServerAppOption(T default_value, std::string_view json_key, std::string_view watcher_path,
                  WatcherMode mode = WatcherMode::kReadOnly)
      : value_(default_value),
        default_(default_value),
        json_key_(json_key),
        watcher_path_(watcher_path),
        mode_(mode) {
    AllOptions().push_back(this);
  }

  ~ServerAppOption() {
    auto& opts = AllOptions();
    opts.erase(std::remove(opts.begin(), opts.end(), this), opts.end());
  }

  // Non-copyable, non-movable (stored by pointer in global list)
  ServerAppOption(const ServerAppOption&) = delete;
  ServerAppOption& operator=(const ServerAppOption&) = delete;

  [[nodiscard]] auto Value() const -> const T& { return value_; }

  void SetValue(const T& v) {
    if (mode_ == WatcherMode::kReadWrite) value_ = v;
  }

  /// Load value from the config's raw DataSection tree.
  /// Called by ServerConfig::apply_options() after JSON loading.
  void LoadFrom(const DataSection& root) {
    if constexpr (std::is_same_v<T, bool>)
      value_ = root.ReadBool(json_key_, default_);
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
      value_ = static_cast<T>(root.ReadInt(json_key_, static_cast<int32_t>(default_)));
    else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>)
      value_ = static_cast<T>(root.ReadUint(json_key_, static_cast<uint32_t>(default_)));
    else if constexpr (std::is_floating_point_v<T>)
      value_ = static_cast<T>(root.ReadFloat(json_key_, static_cast<float>(default_)));
    else
      value_ = static_cast<T>(root.ReadString(json_key_, ""));
  }

  /// Register this option's value as a Watcher entry.
  void RegisterWatcher(WatcherRegistry& registry) {
    if (mode_ == WatcherMode::kReadWrite)
      registry.AddRw(watcher_path_, value_);
    else
      registry.Add(watcher_path_, value_);
  }

  // ---- Global registry ---------------------------------------------------

  /// Process-wide list of all ServerAppOption instances.
  /// Used by ServerConfig::apply_options() and ServerApp::register_watchers().
  static auto AllOptions() -> std::vector<ServerAppOption*>& {
    static std::vector<ServerAppOption*> s_options;
    return s_options;
  }

  /// Apply all registered options from the given DataSection root.
  static void ApplyAll(const DataSection& root) {
    for (auto* opt : AllOptions()) opt->LoadFrom(root);
  }

  /// Register all options into the given WatcherRegistry.
  static void RegisterAll(WatcherRegistry& registry) {
    for (auto* opt : AllOptions()) opt->RegisterWatcher(registry);
  }

 private:
  T value_;
  T default_;
  std::string json_key_;
  std::string watcher_path_;
  WatcherMode mode_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_SERVER_APP_OPTION_H_
