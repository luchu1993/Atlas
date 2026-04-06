#include "platform/threading.hpp"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

namespace atlas
{

// ============================================================================
// ThreadPool::Impl
// ============================================================================

struct ThreadPool::Impl
{
    std::vector<std::jthread>           workers;
    std::queue<std::function<void()>>   tasks;
    std::mutex                          mutex;
    std::condition_variable             cv;
    std::atomic<bool>                   stopped{false};
    std::atomic<uint32_t>               pending{0};
};

// ============================================================================
// ThreadPool
// ============================================================================

ThreadPool::ThreadPool(uint32_t num_threads)
    : impl_(std::make_unique<Impl>())
{
    if (num_threads == 0)
    {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0)
        {
            num_threads = 1;
        }
    }

    impl_->workers.reserve(num_threads);

    for (uint32_t i = 0; i < num_threads; ++i)
    {
        impl_->workers.emplace_back([this](std::stop_token /*stop_token*/)
        {
            while (true)
            {
                std::function<void()> task;

                {
                    std::unique_lock lock(impl_->mutex);
                    impl_->cv.wait(lock, [this]()
                    {
                        return !impl_->tasks.empty()
                               || impl_->stopped.load(std::memory_order_relaxed);
                    });

                    // Exit only when stopped AND no remaining tasks (drain queue)
                    if (impl_->stopped.load(std::memory_order_relaxed)
                        && impl_->tasks.empty())
                    {
                        return;
                    }

                    if (impl_->tasks.empty())
                    {
                        continue;
                    }

                    task = std::move(impl_->tasks.front());
                    impl_->tasks.pop();
                }

                task();
                impl_->pending.fetch_sub(1, std::memory_order_relaxed);
            }
        });
    }
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::enqueue(std::function<void()> task)
{
    {
        std::lock_guard lock(impl_->mutex);
        impl_->tasks.push(std::move(task));
        impl_->pending.fetch_add(1, std::memory_order_relaxed);
    }
    impl_->cv.notify_one();
}

void ThreadPool::shutdown()
{
    if (impl_->stopped.exchange(true, std::memory_order_acq_rel))
    {
        return;  // Already shut down
    }

    // Wake all workers so they see stopped=true and drain remaining tasks
    impl_->cv.notify_all();

    // jthread destructor joins — workers will process all queued tasks
    // before exiting (they only exit when stopped && tasks.empty())
    impl_->workers.clear();
}

auto ThreadPool::thread_count() const -> uint32_t
{
    return static_cast<uint32_t>(impl_->workers.size());
}

auto ThreadPool::pending_tasks() const -> uint32_t
{
    return impl_->pending.load(std::memory_order_relaxed);
}

} // namespace atlas
