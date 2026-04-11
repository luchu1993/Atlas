#include "network/event_dispatcher.hpp"

#include "foundation/log.hpp"

#include <algorithm>

namespace atlas
{

EventDispatcher::EventDispatcher(std::string_view name) : name_(name), poller_(IOPoller::create())
{
}

EventDispatcher::~EventDispatcher() = default;

auto EventDispatcher::register_reader(FdHandle fd, IOCallback callback) -> Result<void>
{
    return poller_->add(fd, IOEvent::Readable, std::move(callback));
}

auto EventDispatcher::register_writer(FdHandle fd, IOCallback callback) -> Result<void>
{
    return poller_->add(fd, IOEvent::Writable, std::move(callback));
}

auto EventDispatcher::modify_interest(FdHandle fd, IOEvent interest) -> Result<void>
{
    return poller_->modify(fd, interest);
}

auto EventDispatcher::deregister(FdHandle fd) -> Result<void>
{
    // Safe to call during poll dispatch — IOPoller implementations swap the
    // callback out for invocation and re-resolve the entry when the map changes.
    return poller_->remove(fd);
}

auto EventDispatcher::add_timer(Duration delay, TimerQueue::Callback callback) -> TimerHandle
{
    return timers_.schedule(Clock::now() + delay, std::move(callback));
}

auto EventDispatcher::add_repeating_timer(Duration interval, TimerQueue::Callback callback)
    -> TimerHandle
{
    return timers_.schedule_repeating(Clock::now() + interval, interval, std::move(callback));
}

auto EventDispatcher::cancel_timer(TimerHandle handle) -> bool
{
    return timers_.cancel(handle);
}

// ============================================================================
// FrequentTaskRegistration out-of-line definitions
// ============================================================================

FrequentTaskRegistration::~FrequentTaskRegistration()
{
    reset();
}

void FrequentTaskRegistration::reset()
{
    if (dispatcher_ && task_)
    {
        dispatcher_->remove_frequent_task(task_);
        dispatcher_ = nullptr;
        task_ = nullptr;
    }
}

// ============================================================================
// Frequent task management
// ============================================================================

auto EventDispatcher::add_frequent_task(FrequentTask* task) -> FrequentTaskRegistration
{
    auto it = std::find(tasks_.begin(), tasks_.end(), task);
    if (it == tasks_.end())
    {
        tasks_.push_back(task);
    }
    return FrequentTaskRegistration(this, task);
}

void EventDispatcher::remove_frequent_task(FrequentTask* task)
{
    if (iterating_tasks_)
    {
        auto it = std::find(tasks_.begin(), tasks_.end(), task);
        if (it != tasks_.end())
        {
            *it = nullptr;
            tasks_dirty_ = true;
        }
    }
    else
    {
        std::erase(tasks_, task);
    }
}

void EventDispatcher::process_frequent_tasks()
{
    iterating_tasks_ = true;
    for (auto* task : tasks_)
    {
        if (task != nullptr)
        {
            task->do_task();
        }
    }
    iterating_tasks_ = false;

    if (tasks_dirty_)
    {
        std::erase(tasks_, nullptr);
        tasks_dirty_ = false;
    }
}

void EventDispatcher::process_once()
{
    process_frequent_tasks();

    auto now = Clock::now();
    timers_.process(now);

    auto time_to_timer = timers_.time_until_next(now);
    auto timeout = std::min({time_to_timer, max_poll_wait_, adaptive_poll_wait_});
    if (timeout < kMinPollWait)
    {
        timeout = kMinPollWait;
    }

    auto result = poller_->poll(timeout);
    if (!result)
    {
        ATLAS_LOG_ERROR("EventDispatcher poll error: {}", result.error().message());
    }

    if (result && *result > 0)
    {
        adaptive_poll_wait_ = kMinPollWait;
    }
    else
    {
        adaptive_poll_wait_ = std::min(adaptive_poll_wait_ * 2, kMaxAdaptivePollWait);
    }

    timers_.process(Clock::now());
}

void EventDispatcher::run()
{
    running_ = true;
    stop_requested_ = false;
    while (!stop_requested_)
    {
        process_once();
    }
    running_ = false;
}

void EventDispatcher::stop()
{
    stop_requested_ = true;
}

void EventDispatcher::set_max_poll_wait(Duration max_wait)
{
    max_poll_wait_ = max_wait;
}

}  // namespace atlas
