#ifndef ATLAS_LIB_FOUNDATION_CONTAINERS_PAGED_SPARSE_TABLE_H_
#define ATLAS_LIB_FOUNDATION_CONTAINERS_PAGED_SPARSE_TABLE_H_

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>

namespace atlas {

template <typename Key, typename T, std::size_t PageBits = 8>
class PagedSparseTable {
  static_assert(std::is_integral_v<Key>, "PagedSparseTable key must be integral");
  static_assert(std::is_unsigned_v<Key>, "PagedSparseTable key must be unsigned");
  static_assert(PageBits > 0, "PageBits must be greater than zero");
  static_assert(PageBits < std::numeric_limits<Key>::digits,
                "PageBits must be smaller than key bit width");

 public:
  static constexpr std::size_t kKeyBits = std::numeric_limits<Key>::digits;
  static constexpr std::size_t kPageBits = PageBits;
  static constexpr std::size_t kPageSize = std::size_t{1} << kPageBits;
  static constexpr std::size_t kTopBits = kKeyBits - kPageBits;
  static constexpr std::size_t kPageCount = std::size_t{1} << kTopBits;
  static constexpr Key kPageMask = static_cast<Key>(kPageSize - 1);
  static constexpr std::size_t kMaxInlinePagePointers = 4096;
  static constexpr std::size_t kMaxPageBytes = size_t{64} * 1024;

  static_assert(kPageCount <= kMaxInlinePagePointers,
                "PagedSparseTable top-level page table would be too large; "
                "use a multi-level sparse table for wider key spaces");

  using ValueType = T;

  [[nodiscard]] auto Insert(Key key, std::unique_ptr<T> value) -> bool {
    auto* slot = this->LocateSlot(key, true);
    if (*slot != nullptr) {
      return false;
    }

    *slot = std::move(value);
    ++size_;
    return true;
  }

  [[nodiscard]] auto Erase(Key key) -> bool {
    auto* slot = this->LocateSlot(key, false);
    if (!slot || *slot == nullptr) {
      return false;
    }

    *slot = nullptr;
    --size_;
    return true;
  }

  [[nodiscard]] auto Get(Key key) -> T* {
    auto* slot = this->LocateSlot(key, false);
    return (slot && *slot) ? slot->get() : nullptr;
  }

  [[nodiscard]] auto Get(Key key) const -> const T* {
    auto* slot = this->LocateSlot(key, false);
    return (slot && *slot) ? slot->get() : nullptr;
  }

  [[nodiscard]] auto Contains(Key key) const -> bool { return this->Get(key) != nullptr; }

  void Clear() {
    for (auto& page : pages_) {
      page.reset();
    }
    size_ = 0;
    allocated_pages_ = 0;
  }

  [[nodiscard]] auto size() const -> std::size_t { return size_; }
  [[nodiscard]] auto empty() const -> bool { return size_ == 0; }
  [[nodiscard]] auto AllocatedPageCount() const -> std::size_t { return allocated_pages_; }

 private:
  using Slot = std::unique_ptr<T>;
  using Page = std::array<Slot, kPageSize>;

  static_assert(sizeof(Page) <= kMaxPageBytes,
                "PagedSparseTable page allocation would be too large; "
                "reduce PageBits or use a multi-level sparse table");

  [[nodiscard]] static constexpr auto PageIndex(Key key) -> std::size_t {
    return static_cast<std::size_t>(key >> kPageBits);
  }

  [[nodiscard]] static constexpr auto SlotIndex(Key key) -> std::size_t {
    return static_cast<std::size_t>(key & kPageMask);
  }

  [[nodiscard]] auto LocateSlot(Key key, bool create_page) -> Slot* {
    auto& page = pages_[PageIndex(key)];
    if (!page) {
      if (!create_page) {
        return nullptr;
      }
      page = std::make_unique<Page>();
      ++allocated_pages_;
    }

    return &((*page)[SlotIndex(key)]);
  }

  [[nodiscard]] auto LocateSlot(Key key, bool create_page) const -> const Slot* {
    const auto& page = pages_[PageIndex(key)];
    if (!page || create_page) {
      return nullptr;
    }

    return &((*page)[SlotIndex(key)]);
  }

  std::array<std::unique_ptr<Page>, kPageCount> pages_{};
  std::size_t size_{0};
  std::size_t allocated_pages_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_CONTAINERS_PAGED_SPARSE_TABLE_H_
