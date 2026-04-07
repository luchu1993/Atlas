#pragma once

#include "script/script_object.hpp"
#include "script/script_value.hpp"

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

    // Custom event system
    void register_listener(std::string_view event, Callback callback);
    void fire_event(std::string_view event, std::span<const ScriptValue> args = {});

    // Access the personality module
    [[nodiscard]] auto module() const -> const std::shared_ptr<ScriptObject>& { return module_; }

private:
    void call_module_method(std::string_view name, std::span<const ScriptValue> args = {});

    std::shared_ptr<ScriptObject> module_;
    std::unordered_map<std::string, std::vector<Callback>> listeners_;
};

}  // namespace atlas
