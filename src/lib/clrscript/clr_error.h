#ifndef ATLAS_LIB_CLRSCRIPT_CLR_ERROR_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_ERROR_H_

#include <cstdint>

#include "foundation/error.h"

namespace atlas {

// [UnmanagedCallersOnly] methods cannot throw managed exceptions; doing so
// crashes the process.  Each C# entry point must catch all exceptions and
// write them into a thread-local buffer via the C# ErrorBridge class.

struct ClrErrorBuffer {
  int32_t error_code{0};
  char message[1024]{};
  int32_t message_length{0};
  bool has_error{false};
};

extern thread_local ClrErrorBuffer t_clr_error;

[[nodiscard]] inline auto HasClrError() -> bool {
  return t_clr_error.has_error;
}

// Must only be called when has_clr_error() is true.
[[nodiscard]] auto ReadClrError() -> Error;

inline void ClearClrError() {
  t_clr_error = ClrErrorBuffer{};
}

// These are invoked from C# via function pointers registered during CLR init,
// not via LibraryImport.

// msg does not need to be null-terminated.
extern "C" void ClrErrorSet(int32_t error_code, const char* msg, int32_t msg_len);

extern "C" void ClrErrorClear();

extern "C" int32_t ClrErrorGetCode();

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_ERROR_H_
