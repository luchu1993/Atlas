#include "network/bg_task_manager.hpp"

#include "foundation/callback_utils.hpp"
#include "foundation/log.hpp"
#include "network/event_dispatcher.hpp"
#include "platform/threading.hpp"

namespace atlas
{

// Lambda adapter for the convenience API
class LambdaTask : public BackgroundTask
{
public:
    LambdaTask(std::function<void()> bg, std::function<void()> main)
        : bg_work_(std::move(bg)), main_callback_(std::move(main))
    {
    }

    void do_background_task() override { bg_work_(); }
    void do_main_thread_task() override { main_callback_(); }

private:
    std::function<void()> bg_work_;
    std::function<void()> main_callback_;
};

struct BgTaskManager::Impl
{
    EventDispatcher& dispatcher;
    ThreadPool pool;
    std::mutex completed_mutex;
    std::vector<std::unique_ptr<BackgroundTask>> completed;
    std::atomic<uint32_t> in_flight{0};

    Impl(EventDispatcher& d, uint32_t threads) : dispatcher(d), pool(threads) {}
};

BgTaskManager::BgTaskManager(EventDispatcher& dispatcher, uint32_t num_threads)
    : impl_(std::make_unique<Impl>(dispatcher, num_threads))
{
    impl_->dispatcher.add_frequent_task(this);
}

BgTaskManager::~BgTaskManager()
{
    impl_->dispatcher.remove_frequent_task(this);
    impl_->pool.shutdown();
    // Process any remaining completed tasks
    do_task();
}

void BgTaskManager::add_task(std::unique_ptr<BackgroundTask> task)
{
    impl_->in_flight.fetch_add(1, std::memory_order_relaxed);  // publish order doesn't matter here

    auto* raw = task.release();  // ThreadPool takes ownership via lambda
    impl_->pool.submit(
        [this, raw]()
        {
            std::unique_ptr<BackgroundTask> owned(raw);
            safe_invoke("BackgroundTask::do_background_task", [&] { owned->do_background_task(); });
            {
                std::lock_guard lock(impl_->completed_mutex);
                impl_->completed.push_back(std::move(owned));
            }
        });
}

void BgTaskManager::add_task(std::function<void()> bg_work, std::function<void()> main_callback)
{
    add_task(std::make_unique<LambdaTask>(std::move(bg_work), std::move(main_callback)));
}

auto BgTaskManager::pending_count() const -> uint32_t
{
    return impl_->pool.pending_tasks();
}

auto BgTaskManager::in_flight_count() const -> uint32_t
{
    // acquire: ensures all worker writes are visible before we read the count
    return impl_->in_flight.load(std::memory_order_acquire);
}

void BgTaskManager::do_task()
{
    std::vector<std::unique_ptr<BackgroundTask>> tasks;
    {
        std::lock_guard lock(impl_->completed_mutex);
        tasks.swap(impl_->completed);
    }

    for (auto& task : tasks)
    {
        safe_invoke("BackgroundTask::do_main_thread_task", [&] { task->do_main_thread_task(); });
        // release: pairs with the acquire in in_flight_count(), ensuring the
        // main thread sees all worker-side mutations before the count drops.
        impl_->in_flight.fetch_sub(1, std::memory_order_release);
    }
}

}  // namespace atlas
