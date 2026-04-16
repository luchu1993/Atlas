#ifndef ATLAS_LIB_CLRSCRIPT_CLR_ERROR_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_ERROR_H_

#include <cstdint>

#include "foundation/error.h"

namespace atlas {

// ============================================================================
// ClrError — C# exception bridging layer
// ============================================================================
//
// [UnmanagedCallersOnly] methods cannot throw managed exceptions; doing so
// crashes the process.  Each C# entry point must catch all exceptions and
// write them into a thread-local buffer via the C# ErrorBridge class.
//
// C++ callers then check has_clr_error() / read_clr_error() after each
// invoke that could fail.
//
// Thread safety:
//   All state is thread_local, so reads and writes from different threads
//   are independent — no locking required.
//
// C# counterpart: Atlas.Core.ErrorBridge (Atlas.Runtime project)
//
// Protocol (per call):
//   1. C++ calls C# [UnmanagedCallersOnly] method, which returns int.
//   2. On success the method returns 0; t_clr_error is untouched.
//   3. On failure the method calls ErrorBridge.SetError(ex) then returns -1.
//   4. C++ checks (result != 0) → read_clr_error() → Error object.
//   5. After a successful call, clear_clr_error() is NOT required (the C#
//      side only writes on failure).  After a failed call, the error is
//      consumed by read_clr_error() which also clears the buffer.

// ============================================================================
// ClrErrorBuffer — thread-local storage for the last C# exception
// ============================================================================

struct ClrErrorBuffer {
  int32_t error_code{0};
  char message[1024]{};
  int32_t message_length{0};
  bool has_error{false};
};

// Access the calling thread's error buffer.  Defined in clr_error.cpp.
extern thread_local ClrErrorBuffer t_clr_error;

// ============================================================================
// C++ API
// ============================================================================

// Returns true if the current thread's error buffer holds an unread error.
[[nodiscard]] inline auto HasClrError() -> bool {
  return t_clr_error.has_error;
}

// Reads the current thread's error, constructs an Error, and clears the buffer.
// Must only be called when has_clr_error() is true.
[[nodiscard]] auto ReadClrError() -> Error;

// Clears the current thread's error buffer.
inline void ClearClrError() {
  t_clr_error = ClrErrorBuffer{};
}

// ============================================================================
// C# → C++ write path (called by the C# ErrorBridge via a function pointer)
// ============================================================================
//
// These are invoked from C# via function pointers registered during CLR init,
// NOT via [LibraryImport] — they live in C++ memory and are not exported
// symbols.  Using direct pointers avoids the symbol-resolution cost at each
// call site.
//
// C++ registers these addresses with C# during bootstrap:
//   ErrorBridgeNative.Register(
//       &clr_error_set, &clr_error_clear, &clr_error_get_code);

// Write an error into the calling thread's buffer.
// msg / msg_len: UTF-8 error message (does not need to be null-terminated).
// error_code   : exception HResult (0 if unavailable).
extern "C" void ClrErrorSet(int32_t error_code, const char* msg, int32_t msg_len);

// Clear the calling thread's error buffer.
extern "C" void ClrErrorClear();

// Return the calling thread's current error code (0 = no error).
extern "C" int32_t ClrErrorGetCode();

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_ERROR_H_
