#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace atlas
{

// Thread safety: Thread-safe (mutex-protected allocate/deallocate).
// Fixed-size block allocator with free-list
class PoolAllocator
{
public:
    explicit PoolAllocator(std::size_t block_size, std::size_t initial_blocks = 64,
                           std::size_t alignment = alignof(std::max_align_t));
    ~PoolAllocator();

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    [[nodiscard]] auto allocate() -> void*;
    void deallocate(void* ptr);

    [[nodiscard]] auto block_size() const -> std::size_t { return block_size_; }
    [[nodiscard]] auto blocks_in_use() const -> std::size_t { return in_use_; }
    [[nodiscard]] auto blocks_total() const -> std::size_t { return total_; }

private:
    struct FreeNode
    {
        FreeNode* next;
    };

    struct Chunk
    {
        Chunk* next;
    };

    // Returns false on OOM instead of throwing (no-exception policy).
    auto grow(std::size_t count) -> bool;

    FreeNode* free_list_{nullptr};
    Chunk* chunks_{nullptr};
    std::size_t block_size_;
    std::size_t alignment_;
    std::size_t in_use_{0};
    std::size_t total_{0};
    std::mutex mutex_;
};

// Typed pool wrapper
template <typename T>
class TypedPool
{
public:
    explicit TypedPool(std::size_t initial_count = 64)
        : pool_(
              // Round block_size up to alignof(T) so every block in the slab
              // is naturally aligned, regardless of Chunk header size.
              (std::max(sizeof(T), sizeof(void*)) + alignof(T) - 1) & ~(alignof(T) - 1),
              initial_count, alignof(T))
    {
    }

    template <typename... Args>
    [[nodiscard]] auto construct(Args&&... args) -> T*
    {
        void* mem = pool_.allocate();
        if (!mem)
            return nullptr;  // OOM — caller must check

        try
        {
            return ::new (mem) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            pool_.deallocate(mem);
            throw;
        }
    }

    void destroy(T* ptr)
    {
        if (ptr)
        {
            ptr->~T();
            pool_.deallocate(ptr);
        }
    }

    [[nodiscard]] auto blocks_in_use() const -> std::size_t { return pool_.blocks_in_use(); }

private:
    PoolAllocator pool_;
};

}  // namespace atlas
