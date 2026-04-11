#include "clrscript/clr_object_registry.hpp"

#include "clrscript/clr_object.hpp"
#include "foundation/log.hpp"

namespace atlas
{

auto ClrObjectRegistry::instance() -> ClrObjectRegistry&
{
    static ClrObjectRegistry s_instance;
    return s_instance;
}

void ClrObjectRegistry::register_object(ClrObject* obj)
{
    std::lock_guard lock(mutex_);
    objects_.insert(obj);
}

void ClrObjectRegistry::unregister_object(ClrObject* obj)
{
    std::lock_guard lock(mutex_);
    objects_.erase(obj);
}

void ClrObjectRegistry::release_all()
{
    std::unordered_set<ClrObject*> snapshot;
    {
        std::lock_guard lock(mutex_);
        if (objects_.empty())
            return;

        ATLAS_LOG_INFO("ClrObjectRegistry: releasing {} tracked objects", objects_.size());
        snapshot.swap(objects_);
    }

    for (auto* obj : snapshot)
        obj->release();
}

auto ClrObjectRegistry::active_count() const -> size_t
{
    std::lock_guard lock(mutex_);
    return objects_.size();
}

}  // namespace atlas
