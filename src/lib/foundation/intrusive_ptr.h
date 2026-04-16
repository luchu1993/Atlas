#ifndef ATLAS_LIB_FOUNDATION_INTRUSIVE_PTR_H_
#define ATLAS_LIB_FOUNDATION_INTRUSIVE_PTR_H_

#include <compare>
#include <functional>
#include <type_traits>
#include <utility>

#include "foundation/ref_counted.h"

namespace atlas {

// Tag for adopting an existing reference (don't increment)
struct adopt_ref_t {
  explicit adopt_ref_t() = default;
};
inline constexpr adopt_ref_t adopt_ref{};

template <IntrusiveRefCounted T>
class IntrusivePtr {
 public:
  constexpr IntrusivePtr() noexcept = default;
  constexpr IntrusivePtr(std::nullptr_t) noexcept {}

  // Acquire: increments ref count
  explicit IntrusivePtr(T* ptr) noexcept : ptr_(ptr) {
    if (ptr_) {
      ptr_->add_ref();
    }
  }

  // Adopt: takes ownership without incrementing
  IntrusivePtr(T* ptr, adopt_ref_t) noexcept : ptr_(ptr) {}

  // Copy
  IntrusivePtr(const IntrusivePtr& other) noexcept : ptr_(other.ptr_) {
    if (ptr_) {
      ptr_->add_ref();
    }
  }

  // Move
  IntrusivePtr(IntrusivePtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

  // Implicit upcast from derived
  template <IntrusiveRefCounted U>
    requires std::is_convertible_v<U*, T*>
  IntrusivePtr(const IntrusivePtr<U>& other) noexcept : ptr_(other.get()) {
    if (ptr_) {
      ptr_->add_ref();
    }
  }

  template <IntrusiveRefCounted U>
    requires std::is_convertible_v<U*, T*>
  IntrusivePtr(IntrusivePtr<U>&& other) noexcept : ptr_(other.detach()) {}

  ~IntrusivePtr() {
    if (ptr_) {
      ptr_->release();
    }
  }

  auto operator=(const IntrusivePtr& other) noexcept -> IntrusivePtr& {
    if (this != &other) {
      IntrusivePtr tmp(other);
      swap(tmp);
    }
    return *this;
  }

  auto operator=(IntrusivePtr&& other) noexcept -> IntrusivePtr& {
    if (this != &other) {
      IntrusivePtr tmp(std::move(other));
      swap(tmp);
    }
    return *this;
  }

  auto operator=(std::nullptr_t) noexcept -> IntrusivePtr& {
    reset();
    return *this;
  }

  [[nodiscard]] auto get() const noexcept -> T* { return ptr_; }
  [[nodiscard]] auto operator->() const noexcept -> T* { return ptr_; }
  [[nodiscard]] auto operator*() const noexcept -> T& { return *ptr_; }
  [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

  void reset(T* ptr = nullptr) {
    IntrusivePtr tmp(ptr);
    swap(tmp);
  }

  // Release ownership without decrementing
  [[nodiscard]] auto detach() noexcept -> T* {
    T* p = ptr_;
    ptr_ = nullptr;
    return p;
  }

  void swap(IntrusivePtr& other) noexcept { std::swap(ptr_, other.ptr_); }

  // Comparison
  [[nodiscard]] auto operator==(const IntrusivePtr& other) const noexcept -> bool {
    return ptr_ == other.ptr_;
  }

  [[nodiscard]] auto operator<=>(const IntrusivePtr& other) const noexcept {
    return ptr_ <=> other.ptr_;
  }

  [[nodiscard]] auto operator==(std::nullptr_t) const noexcept -> bool { return ptr_ == nullptr; }

 private:
  T* ptr_{nullptr};
};

// Factory
template <IntrusiveRefCounted T, typename... Args>
[[nodiscard]] auto make_intrusive(Args&&... args) -> IntrusivePtr<T> {
  return IntrusivePtr<T>(new T(std::forward<Args>(args)...));
}

// Free swap
template <IntrusiveRefCounted T>
void swap(IntrusivePtr<T>& a, IntrusivePtr<T>& b) noexcept {
  a.swap(b);
}

}  // namespace atlas

template <atlas::IntrusiveRefCounted T>
struct std::hash<atlas::IntrusivePtr<T>> {
  auto operator()(const atlas::IntrusivePtr<T>& ptr) const noexcept -> std::size_t {
    return std::hash<T*>{}(ptr.get());
  }
};

#endif  // ATLAS_LIB_FOUNDATION_INTRUSIVE_PTR_H_
