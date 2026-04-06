#pragma once

#include "pyscript/py_object.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace atlas
{

// ============================================================================
// ScriptEvents — Event system that calls Python callbacks on personality module
// ============================================================================
//
// Thread safety: NOT thread-safe. Call from dispatcher thread only.

class ScriptEvents
{
public:
    // Construct with a personality module (imported Python module).
    explicit ScriptEvents(PyObjectPtr personality_module);
    ~ScriptEvents() = default;

    // Standard lifecycle events -- calls module.onInit(is_reload), etc.
    // If the method doesn't exist on the module, silently succeeds.
    void on_init(bool is_reload = false);
    void on_tick(float dt);
    void on_shutdown();

    // Custom event system
    void register_listener(std::string_view event, PyObjectPtr callback);
    void fire_event(std::string_view event, PyObjectPtr args = {});

    // Access the personality module
    [[nodiscard]] auto module() const -> const PyObjectPtr& { return module_; }

private:
    // Helper: call a method on the module, suppress if not found.
    void call_module_method(std::string_view name, PyObjectPtr args = {});

    PyObjectPtr module_;
    std::unordered_map<std::string, std::vector<PyObjectPtr>> listeners_;
};

}  // namespace atlas
