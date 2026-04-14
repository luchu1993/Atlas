#pragma once

#include "network/bg_task_manager.hpp"

#include <coroutine>
#include <optional>
#include <type_traits>
#include <utility>

namespace atlas
{

// co_await await_bg_task(mgr, []{ return heavy_compute(); });
// Runs work on a BgTaskManager thread pool thread, resumes coroutine on the
// main (dispatcher) thread when done.

template <typename F>
    requires(std::invocable<F> && !std::is_void_v<std::invoke_result_t<F>>)
auto await_bg_task(BgTaskManager& mgr, F&& work)
{
    using R = std::invoke_result_t<F>;

    struct Awaiter
    {
        BgTaskManager& mgr;
        F work;
        std::optional<R> result;

        auto await_ready() -> bool { return false; }

        void await_suspend(std::coroutine_handle<> caller)
        {
            mgr.add_task([this]() { result.emplace(work()); },
                         [caller]() mutable { caller.resume(); });
        }

        auto await_resume() -> R { return std::move(*result); }
    };

    return Awaiter{mgr, std::forward<F>(work)};
}

// void specialization
template <typename F>
    requires(std::invocable<F> && std::is_void_v<std::invoke_result_t<F>>)
auto await_bg_task(BgTaskManager& mgr, F&& work)
{
    struct Awaiter
    {
        BgTaskManager& mgr;
        F work;

        auto await_ready() -> bool { return false; }

        void await_suspend(std::coroutine_handle<> caller)
        {
            mgr.add_task([this]() { work(); }, [caller]() mutable { caller.resume(); });
        }

        void await_resume() {}
    };

    return Awaiter{mgr, std::forward<F>(work)};
}

}  // namespace atlas
