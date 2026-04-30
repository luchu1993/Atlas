#ifndef ATLAS_LIB_FOUNDATION_REF_COUNTED_H_
#define ATLAS_LIB_FOUNDATION_REF_COUNTED_H_

#include <atomic>
#include <cstdint>

#include "platform/platform_config.h"

namespace atlas {

struct single_thread_tag {};
struct multi_thread_tag {};

template <typename T>
concept IntrusiveRefCounted = requires(const T& obj) {
  { obj.add_ref() } -> std::same_as<uint32_t>;
  { obj.release() } -> std::same_as<uint32_t>;
  { obj.ref_count() } -> std::same_as<uint32_t>;
};

class RefCounted {
 public:
  using thread_safety_tag = single_thread_tag;
  auto add_ref() const -> uint32_t { return ++ref_count_; }

  auto release() const -> uint32_t {
    auto count = --ref_count_;
    if (count == 0) {
      delete this;
    }
    return count;
  }

  [[nodiscard]] auto ref_count() const -> uint32_t { return ref_count_; }

 protected:
  RefCounted() = default;

  RefCounted(const RefCounted&) noexcept : ref_count_(0) {}
  auto operator=(const RefCounted&) noexcept -> RefCounted& { return *this; }

  virtual ~RefCounted() = default;

 private:
  mutable uint32_t ref_count_{0};
};

class AtomicRefCounted {
 public:
  using thread_safety_tag = multi_thread_tag;
  auto add_ref() const -> uint32_t {
    return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  auto release() const -> uint32_t {
    auto prev = ref_count_.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
      delete this;
      return 0;
    }
    return prev - 1;
  }

  [[nodiscard]] auto ref_count() const -> uint32_t {
    return ref_count_.load(std::memory_order_relaxed);
  }

 protected:
  AtomicRefCounted() = default;
  AtomicRefCounted(const AtomicRefCounted&) noexcept : ref_count_(0) {}
  auto operator=(const AtomicRefCounted&) noexcept -> AtomicRefCounted& { return *this; }
  virtual ~AtomicRefCounted() = default;

 private:
  mutable std::atomic<uint32_t> ref_count_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_REF_COUNTED_H_
