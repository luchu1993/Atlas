#include "foundation/memory_tracker.hpp"

#include <algorithm>

namespace atlas
{

auto MemoryTracker::instance() -> MemoryTracker&
{
    static MemoryTracker tracker;
    return tracker;
}

void MemoryTracker::record_alloc(std::size_t bytes)
{
    std::size_t current = current_bytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    total_allocs_.fetch_add(1, std::memory_order_relaxed);

    std::size_t peak = peak_bytes_.load(std::memory_order_relaxed);
    while (current > peak)
    {
        if (peak_bytes_.compare_exchange_weak(peak, current,
                std::memory_order_relaxed, std::memory_order_relaxed))
        {
            break;
        }
    }
}

void MemoryTracker::record_dealloc(std::size_t bytes)
{
    current_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
    total_deallocs_.fetch_add(1, std::memory_order_relaxed);
}

auto MemoryTracker::stats() const -> Stats
{
    Stats s;
    s.current_bytes = current_bytes_.load(std::memory_order_relaxed);
    s.peak_bytes = peak_bytes_.load(std::memory_order_relaxed);
    s.total_allocations = total_allocs_.load(std::memory_order_relaxed);
    s.total_deallocations = total_deallocs_.load(std::memory_order_relaxed);
    return s;
}

void MemoryTracker::reset()
{
    current_bytes_.store(0, std::memory_order_relaxed);
    peak_bytes_.store(0, std::memory_order_relaxed);
    total_allocs_.store(0, std::memory_order_relaxed);
    total_deallocs_.store(0, std::memory_order_relaxed);
}

} // namespace atlas
