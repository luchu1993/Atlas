#pragma once

#include "platform/platform_config.hpp"

#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string_view>
#include <thread>

namespace atlas
{

// ============================================================================
// Thread naming
// ============================================================================

void set_thread_name(std::string_view name);
void set_thread_name(std::jthread& thread, std::string_view name);

// ============================================================================
// SpinLock -- for very short critical sections
// ============================================================================

class SpinLock
{
public:
    void lock() noexcept
    {
        while (flag_.test_and_set(std::memory_order_acquire))
        {
            // Spin -- optionally add _mm_pause / yield hint
        }
    }

    void unlock() noexcept { flag_.clear(std::memory_order_release); }

    [[nodiscard]] auto try_lock() noexcept -> bool
    {
        return !flag_.test_and_set(std::memory_order_acquire);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// ============================================================================
// ThreadPool
// ============================================================================

class ThreadPool
{
public:
    explicit ThreadPool(uint32_t num_threads = 0);  // 0 = hardware_concurrency
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F, typename... Args>
        requires std::invocable<F, Args...>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();
        enqueue([task = std::move(task)]() { (*task)(); });
        return future;
    }

    [[nodiscard]] auto thread_count() const -> uint32_t;
    [[nodiscard]] auto pending_tasks() const -> uint32_t;

    void shutdown();

private:
    void enqueue(std::function<void()> task);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace atlas
