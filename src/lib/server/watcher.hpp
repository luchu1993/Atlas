#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atlas
{

// ============================================================================
// WatcherMode / WatcherType
// ============================================================================

enum class WatcherMode : uint8_t
{
    ReadOnly,
    ReadWrite,
};

enum class WatcherType : uint8_t
{
    Int64,
    UInt64,
    Double,
    Bool,
    String,
};

// ============================================================================
// WatcherEntry — abstract base for a single observable value
// ============================================================================

class WatcherEntry
{
public:
    virtual ~WatcherEntry() = default;

    [[nodiscard]] virtual auto get_as_string() const -> std::string = 0;

    // Returns false for ReadOnly entries or on parse failure.
    virtual auto set_from_string(std::string_view) -> bool { return false; }

    [[nodiscard]] auto mode() const -> WatcherMode { return mode_; }
    [[nodiscard]] auto type() const -> WatcherType { return type_; }
    [[nodiscard]] auto description() const -> std::string_view { return desc_; }

protected:
    WatcherEntry(WatcherMode mode, WatcherType type, std::string desc)
        : mode_(mode), type_(type), desc_(std::move(desc))
    {
    }

private:
    WatcherMode mode_;
    WatcherType type_;
    std::string desc_;
};

// ============================================================================
// Type traits helpers
// ============================================================================

namespace detail
{

template <typename T>
constexpr auto watcher_type_for() -> WatcherType
{
    if constexpr (std::is_same_v<T, bool>)
        return WatcherType::Bool;
    else if constexpr (std::is_unsigned_v<T>)
        return WatcherType::UInt64;
    else if constexpr (std::is_integral_v<T>)
        return WatcherType::Int64;
    else if constexpr (std::is_floating_point_v<T>)
        return WatcherType::Double;
    else
        return WatcherType::String;
}

template <typename T>
auto value_to_string(const T& v) -> std::string
{
    if constexpr (std::is_same_v<T, bool>)
        return v ? "true" : "false";
    else if constexpr (std::is_arithmetic_v<T>)
        return std::to_string(v);
    else
        return std::string(v);
}

template <typename T>
auto string_to_value(std::string_view s, T& out) -> bool
{
    try
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            if (s == "true" || s == "1")
            {
                out = true;
                return true;
            }
            if (s == "false" || s == "0")
            {
                out = false;
                return true;
            }
            return false;
        }
        else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, int32_t> ||
                           std::is_same_v<T, int16_t> || std::is_same_v<T, int8_t> ||
                           std::is_same_v<T, int>)
        {
            std::size_t pos;
            out = static_cast<T>(std::stoll(std::string(s), &pos));
            return pos == s.size();
        }
        else if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, uint32_t> ||
                           std::is_same_v<T, uint16_t> || std::is_same_v<T, uint8_t> ||
                           std::is_same_v<T, unsigned>)
        {
            std::size_t pos;
            out = static_cast<T>(std::stoull(std::string(s), &pos));
            return pos == s.size();
        }
        else if constexpr (std::is_same_v<T, double>)
        {
            std::size_t pos;
            out = std::stod(std::string(s), &pos);
            return pos == s.size();
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            std::size_t pos;
            out = std::stof(std::string(s), &pos);
            return pos == s.size();
        }
        else
        {
            out = T(s);
            return true;
        }
    }
    catch (...)
    {
        return false;
    }
}

}  // namespace detail

// ============================================================================
// DataWatcher — directly references an existing variable
// ============================================================================

template <typename T>
class DataWatcher final : public WatcherEntry
{
public:
    DataWatcher(const T& ref, WatcherMode mode, std::string desc)
        : WatcherEntry(mode, detail::watcher_type_for<T>(), std::move(desc)),
          ref_(const_cast<T&>(ref))  // const_cast safe: set_from_string guarded by mode
    {
    }

    [[nodiscard]] auto get_as_string() const -> std::string override
    {
        return detail::value_to_string(ref_);
    }

    auto set_from_string(std::string_view s) -> bool override
    {
        if (mode() == WatcherMode::ReadOnly)
            return false;
        return detail::string_to_value(s, ref_);
    }

private:
    T& ref_;
};

// ============================================================================
// FunctionWatcher — value obtained via a getter (and optional setter) lambda
// ============================================================================

template <typename T>
class FunctionWatcher final : public WatcherEntry
{
public:
    FunctionWatcher(std::function<T()> getter, std::function<bool(T)> setter, std::string desc)
        : WatcherEntry(setter ? WatcherMode::ReadWrite : WatcherMode::ReadOnly,
                       detail::watcher_type_for<T>(), std::move(desc)),
          getter_(std::move(getter)),
          setter_(std::move(setter))
    {
    }

    [[nodiscard]] auto get_as_string() const -> std::string override
    {
        return detail::value_to_string(getter_());
    }

    auto set_from_string(std::string_view s) -> bool override
    {
        if (!setter_)
            return false;
        T val{};
        if (!detail::string_to_value(s, val))
            return false;
        return setter_(val);
    }

private:
    std::function<T()> getter_;
    std::function<bool(T)> setter_;
};

// ============================================================================
// WatcherRegistry — hierarchical path-based registry
//
// Owned by ServerApp (not a global singleton) for testability.
// Paths use '/' as separator, e.g. "tick/duration_ms".
// ============================================================================

class WatcherRegistry
{
public:
    WatcherRegistry() = default;

    // Non-copyable, movable
    WatcherRegistry(const WatcherRegistry&) = delete;
    WatcherRegistry& operator=(const WatcherRegistry&) = delete;
    WatcherRegistry(WatcherRegistry&&) = default;
    WatcherRegistry& operator=(WatcherRegistry&&) = default;

    // ---- Registration -------------------------------------------------------

    /// Register a read-only reference to an existing variable.
    template <typename T>
    void add(std::string_view path, const T& ref, std::string_view desc = "")
    {
        insert(path,
               std::make_unique<DataWatcher<T>>(ref, WatcherMode::ReadOnly, std::string(desc)));
    }

    /// Register a read-write reference to an existing variable.
    template <typename T>
    void add_rw(std::string_view path, T& ref, std::string_view desc = "")
    {
        insert(path,
               std::make_unique<DataWatcher<T>>(ref, WatcherMode::ReadWrite, std::string(desc)));
    }

    /// Register a getter-only lambda.
    template <typename T>
    void add(std::string_view path, std::function<T()> getter, std::string_view desc = "")
    {
        insert(path,
               std::make_unique<FunctionWatcher<T>>(std::move(getter), nullptr, std::string(desc)));
    }

    /// Register getter + setter lambdas (ReadWrite).
    template <typename T>
    void add_rw(std::string_view path, std::function<T()> getter, std::function<bool(T)> setter,
                std::string_view desc = "")
    {
        insert(path, std::make_unique<FunctionWatcher<T>>(std::move(getter), std::move(setter),
                                                          std::string(desc)));
    }

    // ---- Query / Mutation ---------------------------------------------------

    [[nodiscard]] auto get(std::string_view path) const -> std::optional<std::string>;

    /// Returns false if path not found, not ReadWrite, or parse failure.
    auto set(std::string_view path, std::string_view value) -> bool;

    /// List all registered paths with the given prefix (empty = all).
    [[nodiscard]] auto list(std::string_view prefix = "") const -> std::vector<std::string>;

    /// Snapshot all current values as (path, value) pairs.
    [[nodiscard]] auto snapshot() const -> std::vector<std::pair<std::string, std::string>>;

    [[nodiscard]] auto size() const -> std::size_t { return count_; }

private:
    struct Node
    {
        std::unique_ptr<WatcherEntry> entry;  // null for directory nodes
        std::map<std::string, Node, std::less<>> children;
    };

    void insert(std::string_view path, std::unique_ptr<WatcherEntry> entry);

    const Node* find_node(std::string_view path) const;
    Node* find_or_create_node(std::string_view path);

    void collect_paths(const Node& node, const std::string& prefix,
                       std::vector<std::string>& out) const;

    void collect_snapshot(const Node& node, const std::string& prefix,
                          std::vector<std::pair<std::string, std::string>>& out) const;

    Node root_;
    std::size_t count_{0};
};

}  // namespace atlas
