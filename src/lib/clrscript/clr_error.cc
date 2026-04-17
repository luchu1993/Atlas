#include "clrscript/clr_error.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace atlas {

// ============================================================================
// Thread-local error storage
// ============================================================================

thread_local ClrErrorBuffer t_clr_error{};

// ============================================================================
// C++ API
// ============================================================================

auto ReadClrError() -> Error {
  // Snapshot the buffer then clear it so subsequent calls don't re-read.
  const int32_t kCode = t_clr_error.error_code;
  const int32_t kLen = t_clr_error.message_length;

  std::string message;
  if (kLen > 0)
    message.assign(t_clr_error.message, static_cast<std::size_t>(kLen));
  else
    message = "CLR exception (no message)";

  ClearClrError();

  return Error{ErrorCode::kScriptError, std::format("CLR exception (HResult=0x{:08X}): {}",
                                                    static_cast<uint32_t>(kCode), message)};
}

// ============================================================================
// C# → C++ write path
// ============================================================================

extern "C" void ClrErrorSet(int32_t error_code, const char* msg, int32_t msg_len) {
  t_clr_error.error_code = error_code;
  t_clr_error.has_error = true;

  // Clamp to buffer capacity (leave no room for null terminator — the buffer
  // is NOT treated as a C string; message_length tracks the byte count).
  constexpr int32_t kBufSize = static_cast<int32_t>(sizeof(ClrErrorBuffer::message));
  const int32_t kCopyLen = std::min(msg_len, kBufSize);

  if (kCopyLen > 0 && msg != nullptr)
    std::memcpy(t_clr_error.message, msg, static_cast<std::size_t>(kCopyLen));

  t_clr_error.message_length = kCopyLen;
}

extern "C" void ClrErrorClear() {
  t_clr_error = ClrErrorBuffer{};
}

extern "C" int32_t ClrErrorGetCode() {
  return t_clr_error.error_code;
}

}  // namespace atlas
