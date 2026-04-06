#pragma once

#include "foundation/time.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace atlas
{

class TimerHandle
{
public:
    constexpr TimerHandle() = default;
    [[nodiscard]] constexpr auto is_valid() const -> bool { return id_ != 0; }
    [[nodiscard]] constexpr auto id() const -> uint64_t { return id_; }
    constexpr auto operator<=>(const TimerHandle&) const = default;

private:
    friend class TimerQueue;
    explicit constexpr TimerHandle(uint64_t id) : id_(id) {}
    uint64_t id_{0};
};

class TimerQueue
{
public:
    using Callback = std::function<void(TimerHandle)>;

    TimerQueue() = default;
    ~TimerQueue();

    // Non-copyable
    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    [[nodiscard]] auto schedule(TimePoint when, Callback callback) -> TimerHandle;
    [[nodiscard]] auto schedule_repeating(TimePoint first_fire, Duration interval, Callback callback) -> TimerHandle;

    auto cancel(TimerHandle handle) -> bool;
    auto process(TimePoint now) -> uint32_t;

    [[nodiscard]] auto time_until_next(TimePoint now) const -> Duration;
    [[nodiscard]] auto size() const -> uint32_t;
    [[nodiscard]] auto empty() const -> bool;

    void clear();

private:
    struct Node
    {
        uint64_t id;
        TimePoint fire_time;
        Duration interval;  // zero for one-shot
        Callback callback;
        bool cancelled{false};
    };

    // Min-heap comparator: earlier fire_time has higher priority
    struct HeapCompare
    {
        auto operator()(const Node* a, const Node* b) const -> bool
        {
            return a->fire_time > b->fire_time;  // std::greater for min-heap
        }
    };

    std::vector<Node*> heap_;
    uint64_t next_id_{1};

    void purge_cancelled();
};

} // namespace atlas
