#pragma once

#include "script/script_object.hpp"
#include "script/script_value.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace atlas
{

// ============================================================================
// ScriptEvents — Language-agnostic event system for personality module callbacks
// ============================================================================
//
// Thread safety: NOT thread-safe. Call from dispatcher thread only.

// Transparent hasher: allows unordered_map<string> to accept string_view keys
// in find() without constructing a temporary std::string (zero heap allocation).
struct StringHash
{
    using is_transparent = void;
    auto operator()(std::string_view sv) const -> std::size_t
    {
        return std::hash<std::string_view>{}(sv);
    }
    auto operator()(const std::string& s) const -> std::size_t
    {
        return std::hash<std::string_view>{}(s);
    }
};

// ============================================================================
// ListenerHandle — opaque token returned by register_listener()
// ============================================================================
//
// Pass to unregister_listener() to remove a specific listener.
// Default-constructed handle is invalid (is_valid() == false).

class ListenerHandle
{
public:
    constexpr ListenerHandle() = default;
    [[nodiscard]] constexpr auto is_valid() const -> bool { return id_ != 0; }
    constexpr auto operator==(const ListenerHandle&) const -> bool = default;

private:
    friend class ScriptEvents;
    explicit constexpr ListenerHandle(uint64_t id) : id_(id) {}
    uint64_t id_{0};
};

class ScriptEvents
{
public:
    using Callback = std::shared_ptr<ScriptObject>;

    explicit ScriptEvents(std::shared_ptr<ScriptObject> personality_module);
    ~ScriptEvents() = default;

    // Standard lifecycle events -- calls module.onInit(is_reload), etc.
    // If the method doesn't exist on the module, silently succeeds.
    void on_init(bool is_reload = false);
    void on_tick(float dt);
    void on_shutdown();

    // Custom event system.
    // register_listener returns a ListenerHandle that can be passed to
    // unregister_listener() to remove the specific listener.
    [[nodiscard]] auto register_listener(std::string_view event, Callback callback)
        -> ListenerHandle;
    auto unregister_listener(ListenerHandle handle) -> bool;
    void fire_event(std::string_view event, std::span<const ScriptValue> args = {});

    // Access the personality module
    [[nodiscard]] auto module() const -> const std::shared_ptr<ScriptObject>& { return module_; }

private:
    void call_module_method(std::string_view name, std::span<const ScriptValue> args = {});

    // Resolve and cache the three standard lifecycle callbacks from module_.
    // Called once at construction; call again after a hot-reload.
    void resolve_cached_methods();

    std::shared_ptr<ScriptObject> module_;

    // Cached references to the three standard lifecycle methods.
    // Null if the module does not export the corresponding attribute.
    std::shared_ptr<ScriptObject> cached_on_init_;
    std::shared_ptr<ScriptObject> cached_on_tick_;
    std::shared_ptr<ScriptObject> cached_on_shutdown_;

    // Listener entries carry an id for O(n) unregister by handle.
    // n is typically very small (< 10 per event), so linear scan is fine.
    struct ListenerEntry
    {
        uint64_t id;
        Callback callback;
    };

    uint64_t next_listener_id_{1};
    // StringHash + equal_to<> enables heterogeneous lookup: find(string_view)
    // resolves without a temporary std::string allocation.
    std::unordered_map<std::string, std::vector<ListenerEntry>, StringHash, std::equal_to<>>
        listeners_;
};

}  // namespace atlas
