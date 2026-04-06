#pragma once

#include "foundation/containers/slot_map.hpp"

namespace atlas
{

template <typename T>
class ObjectPool
{
public:
    using Handle = SlotHandle;

    explicit ObjectPool(std::size_t initial_capacity = 64) : map_(initial_capacity) {}

    template <typename... Args>
    auto create(Args&&... args) -> Handle
    {
        return map_.emplace(std::forward<Args>(args)...);
    }

    void destroy(Handle handle) { map_.remove(handle); }

    [[nodiscard]] auto get(Handle handle) -> T* { return map_.get(handle); }
    [[nodiscard]] auto get(Handle handle) const -> const T* { return map_.get(handle); }
    [[nodiscard]] auto is_valid(Handle handle) const -> bool { return map_.contains(handle); }
    [[nodiscard]] auto size() const -> std::size_t { return map_.size(); }
    [[nodiscard]] auto empty() const -> bool { return map_.empty(); }

    auto begin() { return map_.begin(); }
    auto end() { return map_.end(); }
    auto begin() const { return map_.begin(); }
    auto end() const { return map_.end(); }

private:
    SlotMap<T> map_;
};

}  // namespace atlas
