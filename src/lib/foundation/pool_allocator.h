#ifndef ATLAS_LIB_FOUNDATION_POOL_ALLOCATOR_H_
#define ATLAS_LIB_FOUNDATION_POOL_ALLOCATOR_H_

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace atlas {

// `pool_name` must be a stable pointer (string literal or other static
// storage). Tracy keys named-allocation streams by pointer identity;
// a heap-allocated, transient name would corrupt the trace history of
// any already-connected viewer the first time it was freed and reused.
class PoolAllocator {
 public:
  explicit PoolAllocator(const char* pool_name, std::size_t block_size,
                         std::size_t initial_blocks = 64,
                         std::size_t alignment = alignof(std::max_align_t));
  ~PoolAllocator();

  PoolAllocator(const PoolAllocator&) = delete;
  PoolAllocator& operator=(const PoolAllocator&) = delete;

  [[nodiscard]] auto Allocate() -> void*;
  void Deallocate(void* ptr);

  [[nodiscard]] auto BlockSize() const -> std::size_t { return block_size_; }
  [[nodiscard]] auto BlocksInUse() const -> std::size_t { return in_use_; }
  [[nodiscard]] auto BlocksTotal() const -> std::size_t { return total_; }

 private:
  struct FreeNode {
    FreeNode* next;
  };

  struct Chunk {
    Chunk* next;
  };

  auto Grow(std::size_t count) -> bool;

  FreeNode* free_list_{nullptr};
  Chunk* chunks_{nullptr};
  const char* pool_name_;
  std::size_t block_size_;
  std::size_t alignment_;
  std::size_t in_use_{0};
  std::size_t total_{0};
  std::mutex mutex_;
};

template <typename T>
class TypedPool {
 public:
  explicit TypedPool(const char* pool_name, std::size_t initial_count = 64)
      : pool_(pool_name, (std::max(sizeof(T), sizeof(void*)) + alignof(T) - 1) & ~(alignof(T) - 1),
              initial_count, alignof(T)) {}

  template <typename... Args>
  [[nodiscard]] auto Construct(Args&&... args) -> T* {
    void* mem = pool_.Allocate();
    if (!mem) return nullptr;

    try {
      return ::new (mem) T(std::forward<Args>(args)...);
    } catch (...) {
      pool_.Deallocate(mem);
      throw;
    }
  }

  void Destroy(T* ptr) {
    if (ptr) {
      ptr->~T();
      pool_.Deallocate(ptr);
    }
  }

  [[nodiscard]] auto BlocksInUse() const -> std::size_t { return pool_.BlocksInUse(); }

 private:
  PoolAllocator pool_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_POOL_ALLOCATOR_H_
