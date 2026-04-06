#pragma once

#include "foundation/time.hpp"
#include "foundation/timer_queue.hpp"
#include "network/frequent_task.hpp"
#include "platform/io_poller.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace atlas
{

// Thread safety: NOT thread-safe. All calls must originate from the same thread.
// Callbacks may safely call register/deregister/add_timer/cancel_timer/stop.
class EventDispatcher
{
public:
    explicit EventDispatcher(std::string_view name = "main");
    ~EventDispatcher();

    // Non-copyable, non-movable
    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;

    // IO registration
    [[nodiscard]] auto register_reader(FdHandle fd, IOCallback callback) -> Result<void>;
    [[nodiscard]] auto register_writer(FdHandle fd, IOCallback callback) -> Result<void>;
    [[nodiscard]] auto modify_interest(FdHandle fd, IOEvent interest) -> Result<void>;
    [[nodiscard]] auto deregister(FdHandle fd) -> Result<void>;

    // Timer registration
    [[nodiscard]] auto add_timer(Duration delay, TimerQueue::Callback callback) -> TimerHandle;
    [[nodiscard]] auto add_repeating_timer(Duration interval, TimerQueue::Callback callback)
        -> TimerHandle;
    auto cancel_timer(TimerHandle handle) -> bool;

    // Frequent tasks
    void add_frequent_task(FrequentTask* task);
    void remove_frequent_task(FrequentTask* task);

    // Main loop
    void process_once();
    void run();
    void stop();
    [[nodiscard]] auto is_running() const -> bool { return running_; }

    // Configuration
    void set_max_poll_wait(Duration max_wait);

    // Statistics
    [[nodiscard]] auto name() const -> std::string_view { return name_; }

private:
    void process_frequent_tasks();

    std::string name_;
    std::unique_ptr<IOPoller> poller_;
    TimerQueue timers_;
    std::vector<FrequentTask*> tasks_;
    bool iterating_tasks_{false};

    bool running_{false};
    bool stop_requested_{false};
    Duration max_poll_wait_{Milliseconds(100)};
};

}  // namespace atlas
