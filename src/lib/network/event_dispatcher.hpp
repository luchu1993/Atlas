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

class EventDispatcher;

// ============================================================================
// FrequentTaskRegistration — RAII token returned by add_frequent_task()
// ============================================================================
//
// Destructor automatically calls remove_frequent_task() on the dispatcher.
// Store as a member in objects that register a FrequentTask — they will be
// deregistered when the owner is destroyed, with no manual remove_frequent_task()
// calls needed in the destructor.
//
// IMPORTANT: The registration must be destroyed before the referenced
// EventDispatcher.  Declare it as a member AFTER any dispatcher reference so
// that member destruction order (reverse of declaration) is safe.

class FrequentTaskRegistration
{
public:
    FrequentTaskRegistration() = default;

    ~FrequentTaskRegistration();

    // Move-only: copying would lead to double-removal.
    FrequentTaskRegistration(const FrequentTaskRegistration&) = delete;
    FrequentTaskRegistration& operator=(const FrequentTaskRegistration&) = delete;

    FrequentTaskRegistration(FrequentTaskRegistration&& other) noexcept
        : dispatcher_(other.dispatcher_), task_(other.task_)
    {
        other.dispatcher_ = nullptr;
        other.task_ = nullptr;
    }

    FrequentTaskRegistration& operator=(FrequentTaskRegistration&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            dispatcher_ = other.dispatcher_;
            task_ = other.task_;
            other.dispatcher_ = nullptr;
            other.task_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] auto is_valid() const -> bool { return dispatcher_ != nullptr; }

    // Explicitly release the registration early (calls remove_frequent_task).
    void reset();

private:
    friend class EventDispatcher;
    FrequentTaskRegistration(EventDispatcher* d, FrequentTask* t) : dispatcher_(d), task_(t) {}

    EventDispatcher* dispatcher_{nullptr};
    FrequentTask* task_{nullptr};
};

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

    // Frequent tasks — returns a RAII token that deregisters on destruction.
    // Store the returned FrequentTaskRegistration as a member (declared after
    // any EventDispatcher reference) to guarantee automatic cleanup.
    [[nodiscard]] auto add_frequent_task(FrequentTask* task) -> FrequentTaskRegistration;

    // Internal removal — called by FrequentTaskRegistration::reset().
    // May also be called directly when manual lifetime control is needed.
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
