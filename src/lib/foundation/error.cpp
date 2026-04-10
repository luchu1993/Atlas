#include "foundation/error.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace atlas
{

// ============================================================================
// error_code_name
// ============================================================================

auto error_code_name(ErrorCode code) -> std::string_view
{
    switch (code)
    {
        case ErrorCode::None:
            return "None";
        case ErrorCode::InvalidArgument:
            return "InvalidArgument";
        case ErrorCode::OutOfRange:
            return "OutOfRange";
        case ErrorCode::OutOfMemory:
            return "OutOfMemory";
        case ErrorCode::IoError:
            return "IoError";
        case ErrorCode::Timeout:
            return "Timeout";
        case ErrorCode::NotFound:
            return "NotFound";
        case ErrorCode::AlreadyExists:
            return "AlreadyExists";
        case ErrorCode::PermissionDenied:
            return "PermissionDenied";
        case ErrorCode::NotSupported:
            return "NotSupported";
        case ErrorCode::InternalError:
            return "InternalError";
        case ErrorCode::ConnectionRefused:
            return "ConnectionRefused";
        case ErrorCode::ConnectionReset:
            return "ConnectionReset";
        case ErrorCode::AddressInUse:
            return "AddressInUse";
        case ErrorCode::WouldBlock:
            return "WouldBlock";
        case ErrorCode::NetworkUnreachable:
            return "NetworkUnreachable";
        case ErrorCode::MessageTooLarge:
            return "MessageTooLarge";
        case ErrorCode::RateLimited:
            return "RateLimited";
        case ErrorCode::ChannelCondemned:
            return "ChannelCondemned";
        case ErrorCode::ScriptError:
            return "ScriptError";
        case ErrorCode::ScriptTypeError:
            return "ScriptTypeError";
        case ErrorCode::ScriptValueError:
            return "ScriptValueError";
        case ErrorCode::ScriptImportError:
            return "ScriptImportError";
        case ErrorCode::ScriptRuntimeError:
            return "ScriptRuntimeError";
    }
    return "Unknown";
}

// ============================================================================
// Error
// ============================================================================

Error::Error(ErrorCode code, std::string message) noexcept
    : code_(code), message_(std::move(message))
{
}

auto Error::message() const noexcept -> std::string_view
{
    if (!message_.empty())
    {
        return message_;
    }
    return error_code_name(code_);
}

}  // namespace atlas

// ============================================================================
// Assertion support
// ============================================================================

static std::atomic<AssertHandler> s_assert_handler{nullptr};

void set_assert_handler(AssertHandler handler)
{
    s_assert_handler.store(handler, std::memory_order_release);
}

[[noreturn]] void default_assert_handler(std::string_view expr, std::string_view msg,
                                         std::source_location loc)
{
    auto handler = s_assert_handler.load(std::memory_order_acquire);
    if (handler)
    {
        // Custom handler is a full replacement — it must terminate (abort, throw,
        // or longjmp).  If it returns, we abort with a diagnostic to satisfy the
        // [[noreturn]] contract and prevent silent continuation after an assertion.
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
                 static_cast<int>(expr.size()), expr.data(), static_cast<int>(msg.size()),
                 msg.data(), loc.file_name(), loc.line(), loc.function_name());

#if ATLAS_DEBUG
    ATLAS_DEBUG_BREAK();
#endif
    std::abort();
}
