#include "script/script_events.hpp"

#include "foundation/log.hpp"

#include <algorithm>

namespace atlas
{

ScriptEvents::ScriptEvents(std::shared_ptr<ScriptObject> personality_module)
    : module_(std::move(personality_module))
{
    resolve_cached_methods();
}

void ScriptEvents::resolve_cached_methods()
{
    if (!module_ || module_->is_none())
    {
        cached_on_init_ = nullptr;
        cached_on_tick_ = nullptr;
        cached_on_shutdown_ = nullptr;
        return;
    }

    auto resolve = [&](std::string_view name) -> std::shared_ptr<ScriptObject>
    {
        auto attr = module_->get_attr(name);
        if (attr && attr->is_callable())
            return attr;
        return nullptr;
    };

    cached_on_init_ = resolve("onInit");
    cached_on_tick_ = resolve("onTick");
    cached_on_shutdown_ = resolve("onShutdown");
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
    if (!cached_on_init_)
        return;
    ScriptValue args[] = {ScriptValue(is_reload)};
    auto result = cached_on_init_->call(args);
    if (!result)
        ATLAS_LOG_ERROR("Script callback 'onInit' failed: {}", result.error().message());
    // Re-resolve cached methods after a reload so the new module methods are picked up.
    if (is_reload)
        resolve_cached_methods();
}

void ScriptEvents::on_tick(float dt)
{
    if (!cached_on_tick_)
        return;
    ScriptValue args[] = {ScriptValue::from_float(dt)};
    auto result = cached_on_tick_->call(args);
    if (!result)
        ATLAS_LOG_ERROR("Script callback 'onTick' failed: {}", result.error().message());
}

void ScriptEvents::on_shutdown()
{
    if (!cached_on_shutdown_)
        return;
    auto result = cached_on_shutdown_->call({});
    if (!result)
        ATLAS_LOG_ERROR("Script callback 'onShutdown' failed: {}", result.error().message());
}

auto ScriptEvents::register_listener(std::string_view event, Callback callback) -> ListenerHandle
{
    if (!callback || !callback->is_callable())
    {
        ATLAS_LOG_WARNING("ScriptEvents: ignoring non-callable listener for '{}'", event);
        return ListenerHandle{};
    }
    uint64_t id = next_listener_id_++;
    listeners_[std::string(event)].push_back({id, std::move(callback)});
    return ListenerHandle{id};
}

auto ScriptEvents::unregister_listener(ListenerHandle handle) -> bool
{
    if (!handle.is_valid())
        return false;

    for (auto& [event, entries] : listeners_)
    {
        auto it = std::find_if(entries.begin(), entries.end(),
                               [id = handle.id_](const ListenerEntry& e) { return e.id == id; });
        if (it != entries.end())
        {
            entries.erase(it);
            return true;
        }
    }
    return false;
}

void ScriptEvents::fire_event(std::string_view event, std::span<const ScriptValue> args)
{
    auto it = listeners_.find(event);  // zero-alloc: StringHash supports string_view lookup
    if (it == listeners_.end())
        return;

    // Snapshot the count before dispatch: new listeners registered during a
    // callback are not called in the same round (prevents infinite loops) and
    // avoids iterator invalidation when push_back reallocates the vector.
    const auto count = it->second.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        auto cb =
            it->second[i].callback;  // copy shared_ptr — keeps object alive across reallocation
        auto result = cb->call(args);
        if (!result)
        {
            ATLAS_LOG_WARNING("Event '{}' listener failed: {}", event, result.error().message());
        }
    }
}

}  // namespace atlas
