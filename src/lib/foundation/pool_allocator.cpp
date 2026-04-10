#include "foundation/pool_allocator.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>

namespace atlas
{

namespace
{
constexpr auto align_up(std::size_t n, std::size_t alignment) -> std::size_t
{
    return (n + alignment - 1) & ~(alignment - 1);
}
}  // namespace

PoolAllocator::PoolAllocator(std::size_t block_size, std::size_t initial_blocks,
                             std::size_t alignment)
    : alignment_(alignment < alignof(FreeNode) ? alignof(FreeNode) : alignment)
{
    // block_size must be at least sizeof(FreeNode) AND a multiple of alignment
    // so that every block in the slab starts at an aligned address.
    std::size_t min_size = block_size < sizeof(FreeNode) ? sizeof(FreeNode) : block_size;
    block_size_ = align_up(min_size, alignment_);
    grow(initial_blocks);
}

PoolAllocator::~PoolAllocator()
{
    // In debug builds, catch callers that destroy the pool while blocks are
    // still live.  This indicates a lifetime bug in the caller.
    assert(in_use_ == 0 && "PoolAllocator destroyed with blocks still in use");

    Chunk* chunk = chunks_;
    while (chunk)
    {
        Chunk* next = chunk->next;
        std::free(chunk);
        chunk = next;
    }
}

auto PoolAllocator::grow(std::size_t count) -> bool
{
    // Round the chunk header size up to alignment_ so the first block starts
    // at an aligned offset (malloc guarantees alignof(max_align_t) = 16 bytes
    // for the raw allocation, but sizeof(Chunk) = 8 which would misalign types
    // with alignof > 8 without this adjustment).
    std::size_t header_size = align_up(sizeof(Chunk), alignment_);
    std::size_t raw_size = header_size + block_size_ * count;
    void* raw = std::malloc(raw_size);
    if (!raw)
        return false;  // OOM — caller decides what to do

    Chunk* chunk = static_cast<Chunk*>(raw);
    chunk->next = chunks_;
    chunks_ = chunk;

    char* start = reinterpret_cast<char*>(chunk) + header_size;
    for (std::size_t i = 0; i < count; ++i)
    {
        FreeNode* node = reinterpret_cast<FreeNode*>(start + i * block_size_);
        node->next = free_list_;
        free_list_ = node;
    }

    total_ += count;
    return true;
}

auto PoolAllocator::allocate() -> void*
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!free_list_)
    {
        std::size_t grow_count = total_ > 0 ? total_ : 64;
        if (!grow(grow_count))
            return nullptr;  // OOM
    }

    FreeNode* node = free_list_;
    free_list_ = node->next;
    ++in_use_;

    return static_cast<void*>(node);
}

void PoolAllocator::deallocate(void* ptr)
{
    if (!ptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    FreeNode* node = static_cast<FreeNode*>(ptr);
    node->next = free_list_;
    free_list_ = node;
    --in_use_;
}

}  // namespace atlas
