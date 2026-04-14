#pragma once

#include "coro/cancellation.hpp"
#include "foundation/error.hpp"
#include "foundation/time.hpp"
#include "foundation/timer_queue.hpp"
#include "network/event_dispatcher.hpp"

#include <coroutine>

namespace atlas
{

// co_await async_sleep(dispatcher, Milliseconds(100));
// co_await async_sleep(dispatcher, Milliseconds(100), cancel_token);
// Returns Result<void>: success or Cancelled.
inline auto async_sleep(EventDispatcher& dispatcher, Duration delay, CancellationToken token = {})
{
    struct Awaiter
    {
        EventDispatcher& dispatcher;
        Duration delay;
        CancellationToken token;
        TimerHandle timer_handle{};
        CancelRegistration cancel_reg{};
        bool cancelled{false};

        auto await_ready() -> bool { return token.is_cancelled(); }

        void await_suspend(std::coroutine_handle<> caller)
        {
            timer_handle =
                dispatcher.add_timer(delay, [caller](TimerHandle) mutable { caller.resume(); });

            if (token.is_valid())
            {
                cancel_reg = token.on_cancel(
                    [this, caller]() mutable
                    {
                        cancelled = true;
                        dispatcher.cancel_timer(timer_handle);
                        caller.resume();
                    });
            }
        }

        auto await_resume() -> Result<void>
        {
            if (cancelled || token.is_cancelled())
                return Error{ErrorCode::Cancelled, "sleep cancelled"};
            return {};
        }
    };

    return Awaiter{dispatcher, delay, std::move(token)};
}

}  // namespace atlas
