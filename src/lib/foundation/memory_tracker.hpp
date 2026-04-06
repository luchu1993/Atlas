#pragma once

#include <atomic>
#include <cstddef>

namespace atlas
{

class MemoryTracker
{
public:
    static auto instance() -> MemoryTracker&;

    struct Stats
    {
        std::size_t current_bytes{0};
        std::size_t peak_bytes{0};
        std::size_t total_allocations{0};
        std::size_t total_deallocations{0};
    };

    void record_alloc(std::size_t bytes);
    void record_dealloc(std::size_t bytes);

    [[nodiscard]] auto stats() const -> Stats;
    void reset();

private:
    MemoryTracker() = default;
    std::atomic<std::size_t> current_bytes_{0};
    std::atomic<std::size_t> peak_bytes_{0};
    std::atomic<std::size_t> total_allocs_{0};
    std::atomic<std::size_t> total_deallocs_{0};
};

}  // namespace atlas
