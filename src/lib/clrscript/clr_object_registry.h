#ifndef ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_REGISTRY_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_REGISTRY_H_

#include <cstddef>
#include <mutex>
#include <unordered_set>

namespace atlas {

class ClrObject;

/// Thread-safe registry tracking all ClrObject instances.
/// Used during hot-reload to release all GCHandles before unloading the script assembly.
class ClrObjectRegistry {
 public:
  /// Register a ClrObject for tracking. Called from ClrObject constructor.
  void RegisterObject(ClrObject* obj);

  /// Unregister a ClrObject. Called from ClrObject destructor.
  void UnregisterObject(ClrObject* obj);

  /// Release all tracked ClrObjects (calls release() on each).
  /// After this call, all tracked ClrObjects will have null gc_handle.
  void ReleaseAll();

  /// Number of currently tracked objects.
  [[nodiscard]] auto ActiveCount() const -> size_t;

  /// Process-wide singleton.
  static auto Instance() -> ClrObjectRegistry&;

 private:
  std::unordered_set<ClrObject*> objects_;
  mutable std::mutex mutex_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_REGISTRY_H_
