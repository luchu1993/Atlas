#pragma once

#include "platform/platform_config.hpp"

#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace atlas
{

// ============================================================================
// ErrorCode
// ============================================================================

enum class ErrorCode : uint32_t
{
    None = 0,
    InvalidArgument,
    OutOfRange,
    OutOfMemory,
    IoError,
    Timeout,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    NotSupported,
    InternalError,

    // Network error codes
    ConnectionRefused,
    ConnectionReset,
    AddressInUse,
    WouldBlock,
    NetworkUnreachable,
    MessageTooLarge,
    RateLimited,
    ChannelCondemned,

    // Coroutine error codes
    Cancelled,

    // Script error codes
    ScriptError,
    ScriptTypeError,
    ScriptValueError,
    ScriptImportError,
    ScriptRuntimeError,
};

[[nodiscard]] auto error_code_name(ErrorCode code) -> std::string_view;

// ============================================================================
// Error
// ============================================================================

class Error
{
public:
    constexpr Error() noexcept : code_(ErrorCode::None) {}

    constexpr explicit Error(ErrorCode code) noexcept : code_(code) {}

    Error(ErrorCode code, std::string message) noexcept;

    [[nodiscard]] constexpr auto code() const noexcept -> ErrorCode { return code_; }

    [[nodiscard]] auto message() const noexcept -> std::string_view;

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return code_ != ErrorCode::None;
    }

private:
    ErrorCode code_;
    std::string message_;
};

// ============================================================================
// Result<T, E>
// ============================================================================

template <typename T, typename E = Error>
class Result
{
public:
    // Implicit construction from value or error
    Result(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) : storage_(value) {}

    Result(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) : storage_(std::move(value))
    {
    }

    Result(const E& error) noexcept(std::is_nothrow_copy_constructible_v<E>) : storage_(error) {}

    Result(E&& error) noexcept(std::is_nothrow_move_constructible_v<E>) : storage_(std::move(error))
    {
    }

    [[nodiscard]] auto has_value() const noexcept -> bool
    {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] auto value() & -> T& { return std::get<T>(storage_); }

    [[nodiscard]] auto value() const& -> const T& { return std::get<T>(storage_); }

    [[nodiscard]] auto value() && -> T&& { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] auto error() const& -> const E& { return std::get<E>(storage_); }

    [[nodiscard]] auto operator->() -> T* { return &std::get<T>(storage_); }

    [[nodiscard]] auto operator->() const -> const T* { return &std::get<T>(storage_); }

    [[nodiscard]] auto operator*() & -> T& { return std::get<T>(storage_); }

    [[nodiscard]] auto operator*() const& -> const T& { return std::get<T>(storage_); }

    [[nodiscard]] auto operator*() && -> T&& { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] auto value_or(T default_val) const& -> T
    {
        if (has_value())
        {
            return std::get<T>(storage_);
        }
        return default_val;
    }

    [[nodiscard]] auto value_or(T default_val) && -> T
    {
        if (has_value())
        {
            return std::get<T>(std::move(storage_));
        }
        return default_val;
    }

    template <typename F>
    [[nodiscard]] auto and_then(F&& func) const& -> std::invoke_result_t<F, const T&>
    {
        if (has_value())
        {
            return std::forward<F>(func)(std::get<T>(storage_));
        }
        return std::get<E>(storage_);
    }

    template <typename F>
    [[nodiscard]] auto and_then(F&& func) && -> std::invoke_result_t<F, T&&>
    {
        if (has_value())
        {
            return std::forward<F>(func)(std::get<T>(std::move(storage_)));
        }
        return std::get<E>(std::move(storage_));
    }

    template <typename F>
    [[nodiscard]] auto transform(F&& func) const& -> Result<std::invoke_result_t<F, const T&>, E>
    {
        if (has_value())
        {
            return std::forward<F>(func)(std::get<T>(storage_));
        }
        return std::get<E>(storage_);
    }

    template <typename F>
    [[nodiscard]] auto transform(F&& func) && -> Result<std::invoke_result_t<F, T&&>, E>
    {
        if (has_value())
        {
            return std::forward<F>(func)(std::get<T>(std::move(storage_)));
        }
        return std::get<E>(std::move(storage_));
    }

private:
    std::variant<T, E> storage_;
};

// ============================================================================
// Result<void, E> specialization
// ============================================================================

template <typename E>
class Result<void, E>
{
public:
    // Success: no E is constructed.
    Result() noexcept : storage_(std::in_place_index<0>) {}

    Result(const E& error) noexcept(std::is_nothrow_copy_constructible_v<E>)
        : storage_(std::in_place_index<1>, error)
    {
    }

    Result(E&& error) noexcept(std::is_nothrow_move_constructible_v<E>)
        : storage_(std::in_place_index<1>, std::move(error))
    {
    }

    [[nodiscard]] auto has_value() const noexcept -> bool { return storage_.index() == 0; }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] auto error() const& -> const E& { return std::get<1>(storage_); }

private:
    // index 0 = success (monostate), index 1 = error
    std::variant<std::monostate, E> storage_;
};

}  // namespace atlas

// ============================================================================
// Assertion support
// ============================================================================

using AssertHandler = void (*)(std::string_view expr, std::string_view msg,
                               std::source_location loc);

void set_assert_handler(AssertHandler handler);

[[noreturn]] void default_assert_handler(std::string_view expr, std::string_view msg,
                                         std::source_location loc);

// ============================================================================
// Assertion macros
// ============================================================================

#if ATLAS_DEBUG
#define ATLAS_ASSERT(expr)                                                      \
    do                                                                          \
    {                                                                           \
        if (!(expr))                                                            \
        {                                                                       \
            default_assert_handler(#expr, "", std::source_location::current()); \
        }                                                                       \
    } while (false)

#define ATLAS_ASSERT_MSG(expr, msg)                                                \
    do                                                                             \
    {                                                                              \
        if (!(expr))                                                               \
        {                                                                          \
            default_assert_handler(#expr, (msg), std::source_location::current()); \
        }                                                                          \
    } while (false)
#else
#define ATLAS_ASSERT(expr) ((void)0)
#define ATLAS_ASSERT_MSG(expr, msg) ((void)0)
#endif

#define ATLAS_CHECK(expr, error_val) \
    do                               \
    {                                \
        if (!(expr))                 \
        {                            \
            return (error_val);      \
        }                            \
    } while (false)
