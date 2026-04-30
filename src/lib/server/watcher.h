#ifndef ATLAS_LIB_SERVER_WATCHER_H_
#define ATLAS_LIB_SERVER_WATCHER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atlas {

enum class WatcherMode : uint8_t {
  kReadOnly,
  kReadWrite,
};

enum class WatcherType : uint8_t {
  kInt64,
  kUInt64,
  kDouble,
  kBool,
  kString,
};

class WatcherEntry {
 public:
  virtual ~WatcherEntry() = default;

  [[nodiscard]] virtual auto GetAsString() const -> std::string = 0;

  // Returns false for ReadOnly entries or on parse failure.
  virtual auto SetFromString(std::string_view) -> bool { return false; }

  [[nodiscard]] auto Mode() const -> WatcherMode { return mode_; }
  [[nodiscard]] auto Type() const -> WatcherType { return type_; }
  [[nodiscard]] auto Description() const -> std::string_view { return desc_; }

 protected:
  WatcherEntry(WatcherMode mode, WatcherType type, std::string desc)
      : mode_(mode), type_(type), desc_(std::move(desc)) {}

 private:
  WatcherMode mode_;
  WatcherType type_;
  std::string desc_;
};

namespace detail {

template <typename T>
constexpr auto WatcherTypeFor() -> WatcherType {
  if constexpr (std::is_same_v<T, bool>)
    return WatcherType::kBool;
  else if constexpr (std::is_unsigned_v<T>)
    return WatcherType::kUInt64;
  else if constexpr (std::is_integral_v<T>)
    return WatcherType::kInt64;
  else if constexpr (std::is_floating_point_v<T>)
    return WatcherType::kDouble;
  else
    return WatcherType::kString;
}

template <typename T>
auto ValueToString(const T& v) -> std::string {
  if constexpr (std::is_same_v<T, bool>)
    return v ? "true" : "false";
  else if constexpr (std::is_arithmetic_v<T>)
    return std::to_string(v);
  else
    return std::string(v);
}

template <typename T>
auto StringToValue(std::string_view s, T& out) -> bool {
  try {
    if constexpr (std::is_same_v<T, bool>) {
      if (s == "true" || s == "1") {
        out = true;
        return true;
      }
      if (s == "false" || s == "0") {
        out = false;
        return true;
      }
      return false;
    } else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, int32_t> ||
                         std::is_same_v<T, int16_t> || std::is_same_v<T, int8_t> ||
                         std::is_same_v<T, int>) {
      std::size_t pos;
      out = static_cast<T>(std::stoll(std::string(s), &pos));
      return pos == s.size();
    } else if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, uint32_t> ||
                         std::is_same_v<T, uint16_t> || std::is_same_v<T, uint8_t> ||
                         std::is_same_v<T, unsigned>) {
      std::size_t pos;
      out = static_cast<T>(std::stoull(std::string(s), &pos));
      return pos == s.size();
    } else if constexpr (std::is_same_v<T, double>) {
      std::size_t pos;
      out = std::stod(std::string(s), &pos);
      return pos == s.size();
    } else if constexpr (std::is_same_v<T, float>) {
      std::size_t pos;
      out = std::stof(std::string(s), &pos);
      return pos == s.size();
    } else {
      out = T(s);
      return true;
    }
  } catch (...) {
    return false;
  }
}

}  // namespace detail

template <typename T>
class DataWatcher final : public WatcherEntry {
 public:
  DataWatcher(const T& ref, WatcherMode mode, std::string desc)
      : WatcherEntry(mode, detail::WatcherTypeFor<T>(), std::move(desc)),
        ref_(const_cast<T&>(ref))  // SetFromString is guarded by mode.
  {}

  [[nodiscard]] auto GetAsString() const -> std::string override {
    return detail::ValueToString(ref_);
  }

  auto SetFromString(std::string_view s) -> bool override {
    if (Mode() == WatcherMode::kReadOnly) return false;
    return detail::StringToValue(s, ref_);
  }

 private:
  T& ref_;
};

template <typename T>
class FunctionWatcher final : public WatcherEntry {
 public:
  FunctionWatcher(std::function<T()> getter, std::function<bool(T)> setter, std::string desc)
      : WatcherEntry(setter ? WatcherMode::kReadWrite : WatcherMode::kReadOnly,
                     detail::WatcherTypeFor<T>(), std::move(desc)),
        getter_(std::move(getter)),
        setter_(std::move(setter)) {}

  [[nodiscard]] auto GetAsString() const -> std::string override {
    return detail::ValueToString(getter_());
  }

  auto SetFromString(std::string_view s) -> bool override {
    if (!setter_) return false;
    T val{};
    if (!detail::StringToValue(s, val)) return false;
    return setter_(val);
  }

 private:
  std::function<T()> getter_;
  std::function<bool(T)> setter_;
};

class WatcherRegistry {
 public:
  WatcherRegistry() = default;

  WatcherRegistry(const WatcherRegistry&) = delete;
  WatcherRegistry& operator=(const WatcherRegistry&) = delete;
  WatcherRegistry(WatcherRegistry&&) = default;
  WatcherRegistry& operator=(WatcherRegistry&&) = default;

  template <typename T>
  void Add(std::string_view path, const T& ref, std::string_view desc = "") {
    insert(path, std::make_unique<DataWatcher<T>>(ref, WatcherMode::kReadOnly, std::string(desc)));
  }

  template <typename T>
  void AddRw(std::string_view path, T& ref, std::string_view desc = "") {
    insert(path, std::make_unique<DataWatcher<T>>(ref, WatcherMode::kReadWrite, std::string(desc)));
  }

  template <typename T>
  void Add(std::string_view path, std::function<T()> getter, std::string_view desc = "") {
    insert(path,
           std::make_unique<FunctionWatcher<T>>(std::move(getter), nullptr, std::string(desc)));
  }

  template <typename T>
  void AddRw(std::string_view path, std::function<T()> getter, std::function<bool(T)> setter,
             std::string_view desc = "") {
    insert(path, std::make_unique<FunctionWatcher<T>>(std::move(getter), std::move(setter),
                                                      std::string(desc)));
  }

  [[nodiscard]] auto Get(std::string_view path) const -> std::optional<std::string>;

  auto Set(std::string_view path, std::string_view value) -> bool;

  [[nodiscard]] auto List(std::string_view prefix = "") const -> std::vector<std::string>;

  [[nodiscard]] auto Snapshot() const -> std::vector<std::pair<std::string, std::string>>;

  [[nodiscard]] auto size() const -> std::size_t { return count_; }

 private:
  struct Node {
    std::unique_ptr<WatcherEntry> entry;  // null for directory nodes
    std::map<std::string, Node, std::less<>> children;
  };

  void insert(std::string_view path, std::unique_ptr<WatcherEntry> entry);

  const Node* FindNode(std::string_view path) const;
  Node* FindOrCreateNode(std::string_view path);

  void CollectPaths(const Node& node, const std::string& prefix,
                    std::vector<std::string>& out) const;

  void CollectSnapshot(const Node& node, const std::string& prefix,
                       std::vector<std::pair<std::string, std::string>>& out) const;

  Node root_;
  std::size_t count_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_WATCHER_H_
