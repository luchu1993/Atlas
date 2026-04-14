#pragma once

#include "network/bg_task_manager.hpp"

#include <coroutine>
#include <exception>
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
        std::exception_ptr exception;

        auto await_ready() -> bool { return false; }

        void await_suspend(std::coroutine_handle<> caller)
        {
            mgr.add_task(
                [this]()
                {
                    try
                    {
                        result.emplace(work());
                    }
                    catch (...)
                    {
                        exception = std::current_exception();
                    }
                },
                [caller]() mutable { caller.resume(); });
        }

        auto await_resume() -> R
        {
            if (exception)
                std::rethrow_exception(exception);
            return std::move(*result);
        }
    };

    return Awaiter{mgr, std::forward<F>(work), std::nullopt, nullptr};
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
        std::exception_ptr exception;

        auto await_ready() -> bool { return false; }

        void await_suspend(std::coroutine_handle<> caller)
        {
            mgr.add_task(
                [this]()
                {
                    try
                    {
                        work();
                    }
                    catch (...)
                    {
                        exception = std::current_exception();
                    }
                },
                [caller]() mutable { caller.resume(); });
        }

        void await_resume()
        {
            if (exception)
                std::rethrow_exception(exception);
        }
    };

    return Awaiter{mgr, std::forward<F>(work), nullptr};
}

}  // namespace atlas
