#include "script/script_events.hpp"

#include "foundation/log.hpp"

namespace atlas
{

ScriptEvents::ScriptEvents(std::shared_ptr<ScriptObject> personality_module)
    : module_(std::move(personality_module))
{
}

void ScriptEvents::call_module_method(std::string_view name, std::span<const ScriptValue> args)
{
    if (!module_ || module_->is_none())
        return;

    auto method = module_->get_attr(name);
    if (!method || !method->is_callable())
        return;

    auto result = method->call(args);
    if (!result)
    {
        ATLAS_LOG_ERROR("Script callback '{}' failed: {}", name, result.error().message());
    }
}

void ScriptEvents::on_init(bool is_reload)
{
    ScriptValue args[] = {ScriptValue(is_reload)};
    call_module_method("onInit", args);
}

void ScriptEvents::on_tick(float dt)
{
    ScriptValue args[] = {ScriptValue::from_float(dt)};
    call_module_method("onTick", args);
}

void ScriptEvents::on_shutdown()
{
    call_module_method("onShutdown");
}

void ScriptEvents::register_listener(std::string_view event, Callback callback)
{
    if (!callback || !callback->is_callable())
    {
        ATLAS_LOG_WARNING("ScriptEvents: ignoring non-callable listener for '{}'", event);
        return;
    }
    listeners_[std::string(event)].push_back(std::move(callback));
}

void ScriptEvents::fire_event(std::string_view event, std::span<const ScriptValue> args)
{
    auto it = listeners_.find(std::string(event));
    if (it == listeners_.end())
        return;

    // Snapshot the count before dispatch: new listeners registered during a
    // callback are not called in the same round (prevents infinite loops) and
    // avoids iterator invalidation when push_back reallocates the vector.
    const auto count = it->second.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        auto cb = it->second[i];  // copy shared_ptr — keeps object alive across reallocation
        auto result = cb->call(args);
        if (!result)
        {
            ATLAS_LOG_WARNING("Event '{}' listener failed: {}", event, result.error().message());
        }
    }
}

}  // namespace atlas
