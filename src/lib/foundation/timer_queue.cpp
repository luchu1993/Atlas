#include "foundation/timer_queue.hpp"

#include <algorithm>
#include <exception>

namespace atlas
{

TimerQueue::~TimerQueue()
{
    for (auto* node : heap_)
    {
        delete node;
    }
}

auto TimerQueue::schedule(TimePoint when, Callback callback) -> TimerHandle
{
    auto* node = new Node{next_id_++, when, Duration::zero(), std::move(callback), false};
    heap_.push_back(node);
    std::push_heap(heap_.begin(), heap_.end(), HeapCompare{});
    return TimerHandle(node->id);
}

auto TimerQueue::schedule_repeating(TimePoint first_fire, Duration interval, Callback callback) -> TimerHandle
{
    auto* node = new Node{next_id_++, first_fire, interval, std::move(callback), false};
    heap_.push_back(node);
    std::push_heap(heap_.begin(), heap_.end(), HeapCompare{});
    return TimerHandle(node->id);
}

auto TimerQueue::cancel(TimerHandle handle) -> bool
{
    for (auto* node : heap_)
    {
        if (node->id == handle.id())
        {
            node->cancelled = true;
            return true;
        }
    }
    return false;
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
            delete node;
            continue;
        }

        try
        {
            node->callback(TimerHandle(node->id));
        }
        catch (...)
        {
            delete node;
            throw;
        }

        if (node->interval > Duration::zero())
        {
            node->fire_time += node->interval;
            heap_.push_back(node);
            std::push_heap(heap_.begin(), heap_.end(), HeapCompare{});
        }
        else
        {
            delete node;
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
        delete node;
    }
    heap_.clear();
}

void TimerQueue::purge_cancelled()
{
    // Handled inline during process()
}

} // namespace atlas
