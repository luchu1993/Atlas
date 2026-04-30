#ifndef ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_REGISTRY_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_REGISTRY_H_

#include <cstddef>
#include <mutex>
#include <unordered_set>

namespace atlas {

class ClrObject;

class ClrObjectRegistry {
 public:
  void RegisterObject(ClrObject* obj);

  void UnregisterObject(ClrObject* obj);

  void ReleaseAll();

  [[nodiscard]] auto ActiveCount() const -> size_t;

  static auto Instance() -> ClrObjectRegistry&;

 private:
  std::unordered_set<ClrObject*> objects_;
  mutable std::mutex mutex_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_REGISTRY_H_
