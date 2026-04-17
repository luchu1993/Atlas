#ifndef ATLAS_LIB_FOUNDATION_ERROR_H_
#define ATLAS_LIB_FOUNDATION_ERROR_H_

#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "platform/platform_config.h"

namespace atlas {

// ============================================================================
// ErrorCode
// ============================================================================

enum class ErrorCode : uint32_t {
  kNone = 0,
  kInvalidArgument,
  kOutOfRange,
  kOutOfMemory,
  kIoError,
  kTimeout,
  kNotFound,
  kAlreadyExists,
  kPermissionDenied,
  kNotSupported,
  kInternalError,

  // Network error codes
  kConnectionRefused,
  kConnectionReset,
  kAddressInUse,
  kWouldBlock,
  kNetworkUnreachable,
  kMessageTooLarge,
  kRateLimited,
  kChannelCondemned,

  // Coroutine error codes
  kCancelled,

  // Script error codes
  kScriptError,
  kScriptTypeError,
  kScriptValueError,
  kScriptImportError,
  kScriptRuntimeError,
};

[[nodiscard]] auto ErrorCodeName(ErrorCode code) -> std::string_view;

// ============================================================================
// Error
// ============================================================================

class Error {
 public:
  constexpr Error() noexcept : code_(ErrorCode::kNone) {}

  constexpr explicit Error(ErrorCode code) noexcept : code_(code) {}

  Error(ErrorCode code, std::string message) noexcept;

  [[nodiscard]] constexpr auto Code() const noexcept -> ErrorCode { return code_; }

  [[nodiscard]] auto Message() const noexcept -> std::string_view;

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return code_ != ErrorCode::kNone;
  }

 private:
  ErrorCode code_;
  std::string message_;
};

// ============================================================================
// Result<T, E>
// ============================================================================

template <typename T, typename E = Error>
class Result {
 public:
  // NOLINTBEGIN(google-explicit-constructor)
  // Implicit construction from value or error — intentional for Result ergonomics.
  Result(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) : storage_(value) {}

  Result(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
      : storage_(std::move(value)) {}

  Result(const E& error) noexcept(std::is_nothrow_copy_constructible_v<E>) : storage_(error) {}

  Result(E&& error) noexcept(std::is_nothrow_move_constructible_v<E>)
      : storage_(std::move(error)) {}
  // NOLINTEND(google-explicit-constructor)

  [[nodiscard]] auto HasValue() const noexcept -> bool {
    return std::holds_alternative<T>(storage_);
  }

  [[nodiscard]] explicit operator bool() const noexcept { return HasValue(); }

  [[nodiscard]] auto Value() & -> T& { return std::get<T>(storage_); }

  [[nodiscard]] auto Value() const& -> const T& { return std::get<T>(storage_); }

  [[nodiscard]] auto Value() && -> T&& { return std::get<T>(std::move(storage_)); }

  [[nodiscard]] auto Error() const& -> const E& { return std::get<E>(storage_); }

  [[nodiscard]] auto operator->() -> T* { return &std::get<T>(storage_); }

  [[nodiscard]] auto operator->() const -> const T* { return &std::get<T>(storage_); }

  [[nodiscard]] auto operator*() & -> T& { return std::get<T>(storage_); }

  [[nodiscard]] auto operator*() const& -> const T& { return std::get<T>(storage_); }

  [[nodiscard]] auto operator*() && -> T&& { return std::get<T>(std::move(storage_)); }

  [[nodiscard]] auto ValueOr(T default_val) const& -> T {
    if (HasValue()) {
      return std::get<T>(storage_);
    }
    return default_val;
  }

  [[nodiscard]] auto ValueOr(T default_val) && -> T {
    if (HasValue()) {
      return std::get<T>(std::move(storage_));
    }
    return default_val;
  }

  template <typename F>
  [[nodiscard]] auto AndThen(F&& func) const& -> std::invoke_result_t<F, const T&> {
    if (HasValue()) {
      return std::forward<F>(func)(std::get<T>(storage_));
    }
    return std::get<E>(storage_);
  }

  template <typename F>
  [[nodiscard]] auto AndThen(F&& func) && -> std::invoke_result_t<F, T&&> {
    if (HasValue()) {
      return std::forward<F>(func)(std::get<T>(std::move(storage_)));
    }
    return std::get<E>(std::move(storage_));
  }

  template <typename F>
  [[nodiscard]] auto Transform(F&& func) const& -> Result<std::invoke_result_t<F, const T&>, E> {
    if (HasValue()) {
      return std::forward<F>(func)(std::get<T>(storage_));
    }
    return std::get<E>(storage_);
  }

  template <typename F>
  [[nodiscard]] auto Transform(F&& func) && -> Result<std::invoke_result_t<F, T&&>, E> {
    if (HasValue()) {
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
class Result<void, E> {
 public:
  // Success: no E is constructed.
  Result() noexcept : storage_(std::in_place_index<0>) {}

  // NOLINTBEGIN(google-explicit-constructor)
  Result(const E& error) noexcept(std::is_nothrow_copy_constructible_v<E>)
      : storage_(std::in_place_index<1>, error) {}

  Result(E&& error) noexcept(std::is_nothrow_move_constructible_v<E>)
      : storage_(std::in_place_index<1>, std::move(error)) {}
  // NOLINTEND(google-explicit-constructor)

  [[nodiscard]] auto HasValue() const noexcept -> bool { return storage_.index() == 0; }

  [[nodiscard]] explicit operator bool() const noexcept { return HasValue(); }

  [[nodiscard]] auto Error() const& -> const E& { return std::get<1>(storage_); }

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

void SetAssertHandler(AssertHandler handler);

[[noreturn]] void DefaultAssertHandler(std::string_view expr, std::string_view msg,
                                       std::source_location loc);

// ============================================================================
// Assertion macros
// ============================================================================

#if ATLAS_DEBUG
#define ATLAS_ASSERT(expr)                                              \
  do {                                                                  \
    if (!(expr)) {                                                      \
      DefaultAssertHandler(#expr, "", std::source_location::current()); \
    }                                                                   \
  } while (false)

#define ATLAS_ASSERT_MSG(expr, msg)                                        \
  do {                                                                     \
    if (!(expr)) {                                                         \
      DefaultAssertHandler(#expr, (msg), std::source_location::current()); \
    }                                                                      \
  } while (false)
#else
#define ATLAS_ASSERT(expr) ((void)0)
#define ATLAS_ASSERT_MSG(expr, msg) ((void)0)
#endif

#define ATLAS_CHECK(expr, error_val) \
  do {                               \
    if (!(expr)) {                   \
      return (error_val);            \
    }                                \
  } while (false)

#endif  // ATLAS_LIB_FOUNDATION_ERROR_H_
