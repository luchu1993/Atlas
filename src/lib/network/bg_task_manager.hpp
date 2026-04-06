#pragma once

#include "network/frequent_task.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace atlas
{

class EventDispatcher;

// Abstract background task interface
class BackgroundTask
{
public:
    virtual ~BackgroundTask() = default;

    // Runs on a thread pool thread. Must NOT access network objects.
    virtual void do_background_task() = 0;

    // Runs on the main (dispatcher) thread after background work completes.
    virtual void do_main_thread_task() = 0;
};

// Thread safety: add_task() is safe to call from any thread.
// do_main_thread_task() callbacks run on the EventDispatcher thread only.
// Must outlive all submitted tasks (destructor waits for completion).
class BgTaskManager : public FrequentTask
{
public:
    explicit BgTaskManager(EventDispatcher& dispatcher, uint32_t num_threads = 4);
    ~BgTaskManager() override;

    BgTaskManager(const BgTaskManager&) = delete;
    BgTaskManager& operator=(const BgTaskManager&) = delete;

    // Submit a background task
    void add_task(std::unique_ptr<BackgroundTask> task);

    // Lambda convenience: bg_work runs on pool, main_callback runs on dispatcher thread
    void add_task(std::function<void()> bg_work, std::function<void()> main_callback);

    // Query
    [[nodiscard]] auto pending_count() const -> uint32_t;
    [[nodiscard]] auto in_flight_count() const -> uint32_t;

private:
    // FrequentTask -- processes completed tasks on main thread
    void do_task() override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace atlas
