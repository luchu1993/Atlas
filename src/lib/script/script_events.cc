#include "script/script_events.h"

#include <algorithm>

#include "foundation/log.h"

namespace atlas {

ScriptEvents::ScriptEvents(std::shared_ptr<ScriptObject> personality_module)
    : module_(std::move(personality_module)) {
  ResolveCachedMethods();
}

void ScriptEvents::ResolveCachedMethods() {
  if (!module_ || module_->IsNone()) {
    cached_on_init_ = nullptr;
    cached_on_tick_ = nullptr;
    cached_on_shutdown_ = nullptr;
    return;
  }

  auto resolve = [&](std::string_view name) -> std::shared_ptr<ScriptObject> {
    auto attr = module_->GetAttr(name);
    if (attr && attr->IsCallable()) return attr;
    return nullptr;
  };

  cached_on_init_ = resolve("onInit");
  cached_on_tick_ = resolve("onTick");
  cached_on_shutdown_ = resolve("onShutdown");
}

void ScriptEvents::CallModuleMethod(std::string_view name, std::span<const ScriptValue> args) {
  if (!module_ || module_->IsNone()) return;

  auto method = module_->GetAttr(name);
  if (!method || !method->IsCallable()) return;

  auto result = method->Call(args);
  if (!result) {
    ATLAS_LOG_ERROR("Script callback '{}' failed: {}", name, result.Error().Message());
  }
}

void ScriptEvents::OnInit(bool is_reload) {
  if (!cached_on_init_) return;
  ScriptValue args[] = {ScriptValue(is_reload)};
  auto result = cached_on_init_->Call(args);
  if (!result) ATLAS_LOG_ERROR("Script callback 'onInit' failed: {}", result.Error().Message());
  // Re-resolve cached methods after a reload so the new module methods are picked up.
  if (is_reload) ResolveCachedMethods();
}

void ScriptEvents::OnTick(float dt) {
  if (!cached_on_tick_) return;
  ScriptValue args[] = {ScriptValue::FromFloat(dt)};
  auto result = cached_on_tick_->Call(args);
  if (!result) ATLAS_LOG_ERROR("Script callback 'onTick' failed: {}", result.Error().Message());
}

void ScriptEvents::OnShutdown() {
  if (!cached_on_shutdown_) return;
  auto result = cached_on_shutdown_->Call({});
  if (!result) ATLAS_LOG_ERROR("Script callback 'onShutdown' failed: {}", result.Error().Message());
}

auto ScriptEvents::RegisterListener(std::string_view event, Callback callback) -> ListenerHandle {
  if (!callback || !callback->IsCallable()) {
    ATLAS_LOG_WARNING("ScriptEvents: ignoring non-callable listener for '{}'", event);
    return ListenerHandle{};
  }
  uint64_t id = next_listener_id_++;
  listeners_[std::string(event)].push_back({id, std::move(callback)});
  return ListenerHandle{id};
}

auto ScriptEvents::UnregisterListener(ListenerHandle handle) -> bool {
  if (!handle.IsValid()) return false;

  for (auto& [event, entries] : listeners_) {
    auto it = std::find_if(entries.begin(), entries.end(),
                           [id = handle.id_](const ListenerEntry& e) { return e.id == id; });
    if (it != entries.end()) {
      entries.erase(it);
      return true;
    }
  }
  return false;
}

void ScriptEvents::FireEvent(std::string_view event, std::span<const ScriptValue> args) {
  auto it = listeners_.find(event);  // zero-alloc: StringHash supports string_view lookup
  if (it == listeners_.end()) return;

  // Snapshot the count before dispatch: new listeners registered during a
  // callback are not called in the same round (prevents infinite loops) and
  // avoids iterator invalidation when push_back reallocates the vector.
  const auto kCount = it->second.size();
  for (std::size_t i = 0; i < kCount; ++i) {
    auto cb = it->second[i].callback;  // copy shared_ptr — keeps object alive across reallocation
    auto result = cb->Call(args);
    if (!result) {
      ATLAS_LOG_WARNING("Event '{}' listener failed: {}", event, result.Error().Message());
    }
  }
}

}  // namespace atlas
