#ifndef ATLAS_LIB_SERVER_SERVER_APP_OPTION_H_
#define ATLAS_LIB_SERVER_SERVER_APP_OPTION_H_

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
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
//       10, "backup_period", "baseapp/backup_period", WatcherMode::kReadWrite};
//
// The static instances register themselves in a process-global list at
// construction time. ServerAppOptionBase::ApplyAll() and
// ServerAppOptionBase::RegisterAll() iterate the list.
// ============================================================================

// Non-template base so the process-global registry is shared across all
// ServerAppOption<T> instantiations. Without this, each template instance
// would have its own static list and ApplyAll/RegisterAll would only see
// options of a single T.
class ServerAppOptionBase {
 public:
  virtual ~ServerAppOptionBase() {
    auto& opts = AllOptions();
    opts.erase(std::remove(opts.begin(), opts.end(), this), opts.end());
  }

  virtual void LoadFrom(const DataSection& root) = 0;
  virtual void RegisterWatcher(WatcherRegistry& registry) = 0;

  /// Process-wide list of all ServerAppOption instances across every T.
  static auto AllOptions() -> std::vector<ServerAppOptionBase*>& {
    static std::vector<ServerAppOptionBase*> s_options;
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

 protected:
  ServerAppOptionBase() { AllOptions().push_back(this); }

  ServerAppOptionBase(const ServerAppOptionBase&) = delete;
  ServerAppOptionBase& operator=(const ServerAppOptionBase&) = delete;
};

template <typename T>
class ServerAppOption : public ServerAppOptionBase {
 public:
  ServerAppOption(T default_value, std::string_view json_key, std::string_view watcher_path,
                  WatcherMode mode = WatcherMode::kReadOnly)
      : value_(default_value),
        default_(default_value),
        json_key_(json_key),
        watcher_path_(watcher_path),
        mode_(mode) {}

  [[nodiscard]] auto Value() const -> const T& { return value_; }

  void SetValue(const T& v) {
    if (mode_ == WatcherMode::kReadWrite) value_ = v;
  }

  /// Load value from the config's raw DataSection tree.
  /// Called by ServerAppOptionBase::ApplyAll() after JSON loading.
  void LoadFrom(const DataSection& root) override {
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
  void RegisterWatcher(WatcherRegistry& registry) override {
    if (mode_ == WatcherMode::kReadWrite)
      registry.AddRw(watcher_path_, value_);
    else
      registry.Add(watcher_path_, value_);
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
