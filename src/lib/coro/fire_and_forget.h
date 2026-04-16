#ifndef ATLAS_LIB_CORO_FIRE_AND_FORGET_H_
#define ATLAS_LIB_CORO_FIRE_AND_FORGET_H_

#include <coroutine>
#include <exception>

#include "foundation/log.h"

namespace atlas {

// ============================================================================
// FireAndForget — eager coroutine, nobody co_awaits the result
// ============================================================================

class FireAndForget {
 public:
  struct promise_type {  // NOLINT(readability-identifier-naming) — required by coroutine protocol
    auto get_return_object() -> FireAndForget { return {}; }
    auto initial_suspend() noexcept -> std::suspend_never { return {}; }
    auto final_suspend() noexcept -> std::suspend_never { return {}; }
    void return_void() {}

    void unhandled_exception() {
      try {
        std::rethrow_exception(std::current_exception());
      } catch (const std::exception& e) {
        ATLAS_LOG_ERROR("FireAndForget coroutine threw: {}", e.what());
      } catch (...) {
        ATLAS_LOG_ERROR("FireAndForget coroutine threw unknown exception");
      }
    }
  };
};

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_FIRE_AND_FORGET_H_
