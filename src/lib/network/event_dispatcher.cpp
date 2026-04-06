#include "network/event_dispatcher.hpp"
#include "foundation/log.hpp"

#include <algorithm>

namespace atlas
{

EventDispatcher::EventDispatcher(std::string_view name)
    : name_(name)
    , poller_(IOPoller::create())
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

auto EventDispatcher::deregister(FdHandle fd) -> Result<void>
{
    if (polling_)
    {
        pending_deregistrations_.push_back(fd);
        return {};
    }
    return poller_->remove(fd);
}

auto EventDispatcher::add_timer(Duration delay, TimerQueue::Callback callback) -> TimerHandle
{
    return timers_.schedule(Clock::now() + delay, std::move(callback));
}

auto EventDispatcher::add_repeating_timer(Duration interval, TimerQueue::Callback callback) -> TimerHandle
{
    return timers_.schedule_repeating(Clock::now() + interval, interval, std::move(callback));
}

auto EventDispatcher::cancel_timer(TimerHandle handle) -> bool
{
    return timers_.cancel(handle);
}

void EventDispatcher::add_frequent_task(FrequentTask* task)
{
    auto it = std::find(tasks_.begin(), tasks_.end(), task);
    if (it == tasks_.end())
    {
        tasks_.push_back(task);
    }
}

void EventDispatcher::remove_frequent_task(FrequentTask* task)
{
    if (iterating_tasks_)
    {
        auto it = std::find(tasks_.begin(), tasks_.end(), task);
        if (it != tasks_.end())
        {
            *it = nullptr;
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

    // Compact nullptrs left by deferred removals
    std::erase(tasks_, nullptr);
}

void EventDispatcher::process_pending_deregistrations()
{
    for (auto fd : pending_deregistrations_)
    {
        // Ignore errors — fd might have been re-added during polling
        (void)poller_->remove(fd);
    }
    pending_deregistrations_.clear();
}

void EventDispatcher::process_once()
{
    // 1. Frequent tasks
    process_frequent_tasks();

    // 2. Timers
    timers_.process(Clock::now());

    // 3. Calculate poll timeout
    auto now = Clock::now();
    auto time_to_timer = timers_.time_until_next(now);
    auto timeout = std::min(time_to_timer, max_poll_wait_);
    if (timeout < Duration::zero())
    {
        timeout = Duration::zero();
    }

    // 4. Poll IO
    polling_ = true;
    auto result = poller_->poll(timeout);
    polling_ = false;

    // 5. Process deferred deregistrations
    process_pending_deregistrations();

    if (!result)
    {
        ATLAS_LOG_ERROR("EventDispatcher poll error: {}", result.error().message());
    }
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

} // namespace atlas
