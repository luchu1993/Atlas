#ifndef ATLAS_LIB_SCRIPT_SCRIPT_EVENTS_H_
#define ATLAS_LIB_SCRIPT_SCRIPT_EVENTS_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "script/script_object.h"
#include "script/script_value.h"

namespace atlas {

// Thread safety: NOT thread-safe. Call from dispatcher thread only.

struct StringHash {
  using is_transparent = void;
  auto operator()(std::string_view sv) const -> std::size_t {
    return std::hash<std::string_view>{}(sv);
  }
  auto operator()(const std::string& s) const -> std::size_t {
    return std::hash<std::string_view>{}(s);
  }
};

class ListenerHandle {
 public:
  constexpr ListenerHandle() = default;
  [[nodiscard]] constexpr auto IsValid() const -> bool { return id_ != 0; }
  constexpr auto operator==(const ListenerHandle&) const -> bool = default;

 private:
  friend class ScriptEvents;
  explicit constexpr ListenerHandle(uint64_t id) : id_(id) {}
  uint64_t id_{0};
};

class ScriptEvents {
 public:
  using Callback = std::shared_ptr<ScriptObject>;

  explicit ScriptEvents(std::shared_ptr<ScriptObject> personality_module);
  ~ScriptEvents() = default;

  void OnInit(bool is_reload = false);
  void OnTick(float dt);
  void OnShutdown();

  [[nodiscard]] auto RegisterListener(std::string_view event, Callback callback) -> ListenerHandle;
  auto UnregisterListener(ListenerHandle handle) -> bool;
  void FireEvent(std::string_view event, std::span<const ScriptValue> args = {});

  [[nodiscard]] auto Module() const -> const std::shared_ptr<ScriptObject>& { return module_; }

 private:
  void CallModuleMethod(std::string_view name, std::span<const ScriptValue> args = {});

  // Resolve and cache the three standard lifecycle callbacks from module_.
  // Called once at construction; call again after a hot-reload.
  void ResolveCachedMethods();

  std::shared_ptr<ScriptObject> module_;

  // Cached references to the three standard lifecycle methods.
  // Null if the module does not export the corresponding attribute.
  std::shared_ptr<ScriptObject> cached_on_init_;
  std::shared_ptr<ScriptObject> cached_on_tick_;
  std::shared_ptr<ScriptObject> cached_on_shutdown_;

  // Listener entries carry an id for O(n) unregister by handle.
  // n is typically very small (< 10 per event), so linear scan is fine.
  struct ListenerEntry {
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

#endif  // ATLAS_LIB_SCRIPT_SCRIPT_EVENTS_H_
