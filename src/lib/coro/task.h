#ifndef ATLAS_LIB_CORO_TASK_H_
#define ATLAS_LIB_CORO_TASK_H_

#include <coroutine>
#include <exception>
#include <utility>
#include <variant>

namespace atlas {

template <typename T>
class Task;

namespace detail {

// FinalAwaiter: symmetric transfer back to the caller on coroutine completion.
struct FinalAwaiter {
  auto await_ready() noexcept -> bool { return false; }

  template <typename Promise>
  auto await_suspend(std::coroutine_handle<Promise> h) noexcept -> std::coroutine_handle<> {
    if (h.promise().continuation) return h.promise().continuation;
    return std::noop_coroutine();
  }

  void await_resume() noexcept {}
};

// PromiseBase<T>: common promise machinery for non-void Task.
template <typename T>
struct PromiseBase {
  std::coroutine_handle<> continuation;

  auto initial_suspend() noexcept -> std::suspend_always { return {}; }
  auto final_suspend() noexcept -> FinalAwaiter { return {}; }

  void unhandled_exception() { result.template emplace<2>(std::current_exception()); }

  auto GetResult() -> T {
    if (auto* ex = std::get_if<2>(&result)) std::rethrow_exception(*ex);
    return std::move(std::get<1>(result));
  }

  // 0 = not set, 1 = value, 2 = exception
  std::variant<std::monostate, T, std::exception_ptr> result;
};

// PromiseBase<void>: specialization for Task<void>.
template <>
struct PromiseBase<void> {
  std::coroutine_handle<> continuation;

  auto initial_suspend() noexcept -> std::suspend_always { return {}; }
  auto final_suspend() noexcept -> FinalAwaiter { return {}; }

  void unhandled_exception() { exception = std::current_exception(); }

  void GetResult() const {
    if (exception) std::rethrow_exception(exception);
  }

  std::exception_ptr exception;
};

}  // namespace detail

// ============================================================================
// Task<T> — lazy coroutine return type (starts only when co_awaited)
// ============================================================================

template <typename T = void>
class [[nodiscard]] Task {
 public:
  struct promise_type : detail::PromiseBase<T> {
    auto get_return_object() -> Task {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    void return_value(T value) { this->result.template emplace<1>(std::move(value)); }
  };

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  ~Task() {
    if (handle_) handle_.destroy();
  }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  auto operator co_await() & { return MakeAwaiter(); }
  auto operator co_await() && { return MakeAwaiter(); }

 private:
  struct Awaiter {
    std::coroutine_handle<promise_type> handle;

    auto await_ready() -> bool { return handle.done(); }

    auto await_suspend(std::coroutine_handle<> caller) noexcept -> std::coroutine_handle<> {
      handle.promise().continuation = caller;
      return handle;  // symmetric transfer
    }

    auto await_resume() -> T { return handle.promise().GetResult(); }
  };

  auto MakeAwaiter() -> Awaiter { return Awaiter{handle_}; }

  explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
  std::coroutine_handle<promise_type> handle_;
};

// ============================================================================
// Task<void> specialization
// ============================================================================

template <>
struct Task<void>::promise_type : detail::PromiseBase<void> {
  auto get_return_object() -> Task<void> {
    return Task<void>{std::coroutine_handle<promise_type>::from_promise(*this)};
  }

  void return_void() {}
};

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_TASK_H_
