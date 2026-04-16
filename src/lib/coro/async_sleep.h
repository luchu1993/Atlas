#ifndef ATLAS_LIB_CORO_ASYNC_SLEEP_H_
#define ATLAS_LIB_CORO_ASYNC_SLEEP_H_

#include <coroutine>

#include "coro/cancellation.h"
#include "foundation/clock.h"
#include "foundation/error.h"
#include "foundation/timer_queue.h"
#include "network/event_dispatcher.h"

namespace atlas {

// co_await async_sleep(dispatcher, Milliseconds(100));
// co_await async_sleep(dispatcher, Milliseconds(100), cancel_token);
// Returns Result<void>: success or Cancelled.
inline auto async_sleep(EventDispatcher& dispatcher, Duration delay, CancellationToken token = {}) {
  struct Awaiter {
    EventDispatcher& dispatcher;
    Duration delay;
    CancellationToken token;
    TimerHandle timer_handle{};
    CancelRegistration cancel_reg{};
    bool cancelled{false};

    auto await_ready() -> bool { return token.IsCancelled(); }

    void await_suspend(std::coroutine_handle<> caller) {
      timer_handle = dispatcher.AddTimer(delay, [caller](TimerHandle) mutable { caller.resume(); });

      if (token.IsValid()) {
        cancel_reg = token.OnCancel([this, caller]() mutable {
          cancelled = true;
          dispatcher.CancelTimer(timer_handle);
          caller.resume();
        });
      }
    }

    auto await_resume() -> Result<void> {
      if (cancelled || token.IsCancelled()) return Error{ErrorCode::kCancelled, "sleep cancelled"};
      return {};
    }
  };

  return Awaiter{dispatcher, delay, std::move(token)};
}

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_ASYNC_SLEEP_H_
