#ifndef ATLAS_LIB_FOUNDATION_CONTAINERS_SLOT_MAP_H_
#define ATLAS_LIB_FOUNDATION_CONTAINERS_SLOT_MAP_H_

#include <cstdint>
#include <vector>

namespace atlas {

struct SlotHandle {
  uint32_t index{~0u};
  uint32_t generation{0};

  [[nodiscard]] constexpr auto IsValid() const -> bool { return index != ~0u; }
  constexpr auto operator<=>(const SlotHandle&) const = default;
};

template <typename T>
class SlotMap {
 public:
  explicit SlotMap(std::size_t initial_capacity = 64) {
    slots_.reserve(initial_capacity);
    dense_.reserve(initial_capacity);
    dense_to_sparse_.reserve(initial_capacity);
  }

  auto Insert(T value) -> SlotHandle {
    uint32_t sparse_index;
    if (free_head_ != kNullIndex) {
      sparse_index = free_head_;
      free_head_ = slots_[sparse_index].next_free;
    } else {
      sparse_index = static_cast<uint32_t>(slots_.size());
      slots_.push_back(Slot{});
    }

    auto& slot = slots_[sparse_index];
    auto dense_index = static_cast<uint32_t>(dense_.size());
    slot.dense_index = dense_index;

    dense_.push_back(std::move(value));
    dense_to_sparse_.push_back(sparse_index);

    return SlotHandle{sparse_index, slot.generation};
  }

  template <typename... Args>
  auto Emplace(Args&&... args) -> SlotHandle {
    return Insert(T(std::forward<Args>(args)...));
  }

  auto Remove(SlotHandle handle) -> bool {
    if (!Contains(handle)) return false;

    auto& slot = slots_[handle.index];
    auto dense_idx = slot.dense_index;

    // Swap with last element in dense array
    if (dense_idx != dense_.size() - 1) {
      auto last_sparse = dense_to_sparse_.back();
      dense_[dense_idx] = std::move(dense_.back());
      dense_to_sparse_[dense_idx] = last_sparse;
      slots_[last_sparse].dense_index = dense_idx;
    }

    dense_.pop_back();
    dense_to_sparse_.pop_back();

    // Recycle slot
    slot.generation++;
    slot.dense_index = kNullIndex;
    slot.next_free = free_head_;
    free_head_ = handle.index;

    return true;
  }

  [[nodiscard]] auto Get(SlotHandle handle) -> T* {
    if (!Contains(handle)) return nullptr;
    return &dense_[slots_[handle.index].dense_index];
  }

  [[nodiscard]] auto Get(SlotHandle handle) const -> const T* {
    if (!Contains(handle)) return nullptr;
    return &dense_[slots_[handle.index].dense_index];
  }

  [[nodiscard]] auto Contains(SlotHandle handle) const -> bool {
    return handle.index < slots_.size() && slots_[handle.index].generation == handle.generation &&
           slots_[handle.index].dense_index != kNullIndex;
  }

  [[nodiscard]] auto size() const -> std::size_t { return dense_.size(); }
  [[nodiscard]] auto empty() const -> bool { return dense_.empty(); }

  void Clear() {
    for (auto& slot : slots_) {
      if (slot.dense_index != kNullIndex) {
        slot.generation++;
        slot.dense_index = kNullIndex;
      }
    }
    dense_.clear();
    dense_to_sparse_.clear();
    // Rebuild free list
    free_head_ = kNullIndex;
    for (uint32_t i = static_cast<uint32_t>(slots_.size()); i > 0; --i) {
      slots_[i - 1].next_free = free_head_;
      free_head_ = i - 1;
    }
  }

  // Dense iteration (no gaps)
  auto begin() { return dense_.begin(); }
  auto end() { return dense_.end(); }
  auto begin() const { return dense_.begin(); }
  auto end() const { return dense_.end(); }

 private:
  static constexpr uint32_t kNullIndex = ~0u;

  struct Slot {
    uint32_t generation{0};
    uint32_t dense_index{kNullIndex};
    uint32_t next_free{kNullIndex};
  };

  std::vector<Slot> slots_;
  std::vector<T> dense_;
  std::vector<uint32_t> dense_to_sparse_;
  uint32_t free_head_{kNullIndex};
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_CONTAINERS_SLOT_MAP_H_
