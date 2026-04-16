#include "clrscript/clr_object_registry.h"

#include "clrscript/clr_object.h"
#include "foundation/log.h"

namespace atlas {

auto ClrObjectRegistry::Instance() -> ClrObjectRegistry& {
  static ClrObjectRegistry s_instance;
  return s_instance;
}

void ClrObjectRegistry::RegisterObject(ClrObject* obj) {
  std::lock_guard lock(mutex_);
  objects_.insert(obj);
}

void ClrObjectRegistry::UnregisterObject(ClrObject* obj) {
  std::lock_guard lock(mutex_);
  objects_.erase(obj);
}

void ClrObjectRegistry::ReleaseAll() {
  std::unordered_set<ClrObject*> snapshot;
  {
    std::lock_guard lock(mutex_);
    if (objects_.empty()) return;

    ATLAS_LOG_INFO("ClrObjectRegistry: releasing {} tracked objects", objects_.size());
    snapshot.swap(objects_);
  }

  for (auto* obj : snapshot) obj->Release();
}

auto ClrObjectRegistry::ActiveCount() const -> size_t {
  std::lock_guard lock(mutex_);
  return objects_.size();
}

}  // namespace atlas
