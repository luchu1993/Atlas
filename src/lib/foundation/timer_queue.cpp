#include "foundation/timer_queue.hpp"

#include "foundation/callback_utils.hpp"
#include "foundation/error.hpp"

#include <algorithm>

namespace atlas
{

TimerQueue::~TimerQueue()
{
    for (auto* node : heap_)
    {
        node_pool_.destroy(node);
    }
}

auto TimerQueue::schedule(TimePoint when, Callback callback) -> TimerHandle
{
    auto* node =
        node_pool_.construct(next_id_++, when, Duration::zero(), std::move(callback), false);
    ATLAS_ASSERT_MSG(node != nullptr, "TimerQueue: node_pool_ OOM");
    heap_.push_back(node);
    std::push_heap(heap_.begin(), heap_.end(), HeapCompare{});
    index_[node->id] = node;
    return TimerHandle(node->id);
}

auto TimerQueue::schedule_repeating(TimePoint first_fire, Duration interval, Callback callback)
    -> TimerHandle
{
    auto* node = node_pool_.construct(next_id_++, first_fire, interval, std::move(callback), false);
    ATLAS_ASSERT_MSG(node != nullptr, "TimerQueue: node_pool_ OOM");
    heap_.push_back(node);
    std::push_heap(heap_.begin(), heap_.end(), HeapCompare{});
    index_[node->id] = node;
    return TimerHandle(node->id);
}

auto TimerQueue::cancel(TimerHandle handle) -> bool
{
    // O(1) lookup via index instead of O(N) heap scan
    auto it = index_.find(handle.id());
    if (it == index_.end())
    {
        return false;
    }
    it->second->cancelled = true;
    index_.erase(it);

    // Eagerly purge cancelled nodes from the heap top so that time_until_next()
    // always reflects the earliest *valid* timer without needing heap mutation
    // in a const method.  The cost is O(k) where k = consecutive cancelled
    // entries at the front, which is typically 0 or 1.
    while (!heap_.empty() && heap_.front()->cancelled)
    {
        std::pop_heap(heap_.begin(), heap_.end(), HeapCompare{});
        node_pool_.destroy(heap_.back());
        heap_.pop_back();
    }

    return true;
}

auto TimerQueue::process(TimePoint now) -> uint32_t
{
    uint32_t count = 0;

    while (!heap_.empty() && heap_.front()->fire_time <= now)
    {
        std::pop_heap(heap_.begin(), heap_.end(), HeapCompare{});
        auto* node = heap_.back();
        heap_.pop_back();

        if (node->cancelled)
        {
            // Already removed from index_ in cancel()
            node_pool_.destroy(node);
            continue;
        }

        safe_invoke("TimerQueue callback", [&] { node->callback(TimerHandle(node->id)); });

        if (node->interval > Duration::zero())
        {
            node->fire_time += node->interval;
            heap_.push_back(node);
            std::push_heap(heap_.begin(), heap_.end(), HeapCompare{});
            // node stays in index_ — it can still be cancelled
        }
        else
        {
            index_.erase(node->id);
            node_pool_.destroy(node);
        }

        ++count;
    }

    return count;
}

auto TimerQueue::time_until_next(TimePoint now) const -> Duration
{
    if (heap_.empty() || heap_.front()->cancelled)
    {
        return Duration::max();
    }
    auto diff = heap_.front()->fire_time - now;
    return std::max(diff, Duration::zero());
}

auto TimerQueue::size() const -> uint32_t
{
    return static_cast<uint32_t>(heap_.size());
}

auto TimerQueue::empty() const -> bool
{
    return heap_.empty();
}

void TimerQueue::clear()
{
    for (auto* node : heap_)
    {
        node_pool_.destroy(node);
    }
    heap_.clear();
    index_.clear();
}

void TimerQueue::purge_cancelled()
{
    // Handled inline during process()
}

}  // namespace atlas
