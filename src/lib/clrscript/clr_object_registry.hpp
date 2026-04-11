#pragma once

#include <cstddef>
#include <mutex>
#include <unordered_set>

namespace atlas
{

class ClrObject;

/// Thread-safe registry tracking all ClrObject instances.
/// Used during hot-reload to release all GCHandles before unloading the script assembly.
class ClrObjectRegistry
{
public:
    /// Register a ClrObject for tracking. Called from ClrObject constructor.
    void register_object(ClrObject* obj);

    /// Unregister a ClrObject. Called from ClrObject destructor.
    void unregister_object(ClrObject* obj);

    /// Release all tracked ClrObjects (calls release() on each).
    /// After this call, all tracked ClrObjects will have null gc_handle.
    void release_all();

    /// Number of currently tracked objects.
    [[nodiscard]] auto active_count() const -> size_t;

    /// Process-wide singleton.
    static auto instance() -> ClrObjectRegistry&;

private:
    std::unordered_set<ClrObject*> objects_;
    mutable std::mutex mutex_;
};

}  // namespace atlas
