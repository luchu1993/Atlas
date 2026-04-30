#ifndef ATLAS_LIB_FOUNDATION_CALLBACK_UTILS_H_
#define ATLAS_LIB_FOUNDATION_CALLBACK_UTILS_H_

// Runs user callbacks at exception-tolerant boundaries.

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
