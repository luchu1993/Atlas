#ifndef ATLAS_LIB_FOUNDATION_CALLBACK_UTILS_H_
#define ATLAS_LIB_FOUNDATION_CALLBACK_UTILS_H_

// safe_invoke — uniform wrapper for calling user-supplied callbacks that may
// throw, in a project that otherwise avoids exceptions.
//
// Usage:
//   safe_invoke("context description", [&]{ user_callback(); });
//
// On exception: logs a warning and returns without re-throwing.

#include <exception>
#include <utility>

#include "foundation/log.h"

namespace atlas {

template <typename F>
void SafeInvoke(const char* context, F&& fn) noexcept {
  try {
    std::forward<F>(fn)();
  } catch (const std::exception& e) {
    ATLAS_LOG_WARNING("{} threw exception: {}", context, e.what());
  } catch (...) {
    ATLAS_LOG_WARNING("{} threw unknown exception", context);
  }
}

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_CALLBACK_UTILS_H_
