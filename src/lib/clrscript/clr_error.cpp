#include "clrscript/clr_error.hpp"

#include "foundation/log.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace atlas
{

// ============================================================================
// Thread-local error storage
// ============================================================================

thread_local ClrErrorBuffer t_clr_error{};

// ============================================================================
// C++ API
// ============================================================================

auto read_clr_error() -> Error
{
    // Snapshot the buffer then clear it so subsequent calls don't re-read.
    const int32_t code = t_clr_error.error_code;
    const int32_t len = t_clr_error.message_length;

    std::string message;
    if (len > 0)
        message.assign(t_clr_error.message, static_cast<std::size_t>(len));
    else
        message = "CLR exception (no message)";

    clear_clr_error();

    return Error{ErrorCode::ScriptError, std::format("CLR exception (HResult=0x{:08X}): {}",
                                                     static_cast<uint32_t>(code), message)};
}

// ============================================================================
// C# → C++ write path
// ============================================================================

extern "C" void clr_error_set(int32_t error_code, const char* msg, int32_t msg_len)
{
    t_clr_error.error_code = error_code;
    t_clr_error.has_error = true;

    // Clamp to buffer capacity (leave no room for null terminator — the buffer
    // is NOT treated as a C string; message_length tracks the byte count).
    constexpr int32_t kBufSize = static_cast<int32_t>(sizeof(ClrErrorBuffer::message));
    const int32_t copy_len = std::min(msg_len, kBufSize);

    if (copy_len > 0 && msg != nullptr)
        std::memcpy(t_clr_error.message, msg, static_cast<std::size_t>(copy_len));

    t_clr_error.message_length = copy_len;
}

extern "C" void clr_error_clear()
{
    t_clr_error = ClrErrorBuffer{};
}

extern "C" int32_t clr_error_get_code()
{
    return t_clr_error.error_code;
}

}  // namespace atlas
