#include "foundation/error.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace atlas {

auto ErrorCodeName(ErrorCode code) -> std::string_view {
  switch (code) {
    case ErrorCode::kNone:
      return "None";
    case ErrorCode::kInvalidArgument:
      return "InvalidArgument";
    case ErrorCode::kOutOfRange:
      return "OutOfRange";
    case ErrorCode::kOutOfMemory:
      return "OutOfMemory";
    case ErrorCode::kIoError:
      return "IoError";
    case ErrorCode::kTimeout:
      return "Timeout";
    case ErrorCode::kNotFound:
      return "NotFound";
    case ErrorCode::kAlreadyExists:
      return "AlreadyExists";
    case ErrorCode::kPermissionDenied:
      return "PermissionDenied";
    case ErrorCode::kNotSupported:
      return "NotSupported";
    case ErrorCode::kInternalError:
      return "InternalError";
    case ErrorCode::kConnectionRefused:
      return "ConnectionRefused";
    case ErrorCode::kConnectionReset:
      return "ConnectionReset";
    case ErrorCode::kAddressInUse:
      return "AddressInUse";
    case ErrorCode::kWouldBlock:
      return "WouldBlock";
    case ErrorCode::kNetworkUnreachable:
      return "NetworkUnreachable";
    case ErrorCode::kMessageTooLarge:
      return "MessageTooLarge";
    case ErrorCode::kRateLimited:
      return "RateLimited";
    case ErrorCode::kChannelCondemned:
      return "ChannelCondemned";
    case ErrorCode::kCancelled:
      return "Cancelled";
    case ErrorCode::kReceiverGone:
      return "ReceiverGone";
    case ErrorCode::kScriptError:
      return "ScriptError";
    case ErrorCode::kScriptTypeError:
      return "ScriptTypeError";
    case ErrorCode::kScriptValueError:
      return "ScriptValueError";
    case ErrorCode::kScriptImportError:
      return "ScriptImportError";
    case ErrorCode::kScriptRuntimeError:
      return "ScriptRuntimeError";
  }
  return "Unknown";
}

Error::Error(ErrorCode code, std::string message) noexcept
    : code_(code), message_(std::move(message)) {}

auto Error::Message() const noexcept -> std::string_view {
  if (!message_.empty()) {
    return message_;
  }
  return ErrorCodeName(code_);
}

}  // namespace atlas

static std::atomic<AssertHandler> s_assert_handler{nullptr};

void SetAssertHandler(AssertHandler handler) {
  s_assert_handler.store(handler, std::memory_order_release);
}

[[noreturn]] void DefaultAssertHandler(std::string_view expr, std::string_view msg,
                                       std::source_location loc) {
  auto handler = s_assert_handler.load(std::memory_order_acquire);
  if (handler) {
    // If a custom handler returns, abort to preserve the [[noreturn]] contract.
    handler(expr, msg, loc);
    std::fprintf(stderr,
                 "FATAL: Custom assert handler returned instead of terminating.\n"
                 "  Assertion: %.*s\n",
                 static_cast<int>(expr.size()), expr.data());
    std::abort();
  }

  std::fprintf(stderr,
               "Assertion failed: %.*s\n"
               "  Message:  %.*s\n"
               "  File:     %s\n"
               "  Line:     %u\n"
               "  Function: %s\n",
               static_cast<int>(expr.size()), expr.data(), static_cast<int>(msg.size()), msg.data(),
               loc.file_name(), loc.line(), loc.function_name());

#if ATLAS_DEBUG
  ATLAS_DEBUG_BREAK();
#endif
  std::abort();
}
