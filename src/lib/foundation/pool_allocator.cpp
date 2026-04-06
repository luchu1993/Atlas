#include "foundation/pool_allocator.hpp"

#include <cstdlib>
#include <cstring>

namespace atlas
{

PoolAllocator::PoolAllocator(std::size_t block_size, std::size_t initial_blocks)
    : block_size_(block_size < sizeof(FreeNode) ? sizeof(FreeNode) : block_size)
{
    grow(initial_blocks);
}

PoolAllocator::~PoolAllocator()
{
    Chunk* chunk = chunks_;
    while (chunk)
    {
        Chunk* next = chunk->next;
        std::free(chunk);
        chunk = next;
    }
}

void PoolAllocator::grow(std::size_t count)
{
    std::size_t raw_size = sizeof(Chunk) + block_size_ * count;
    void* raw = std::malloc(raw_size);
    if (!raw)
    {
        throw std::bad_alloc();
    }

    Chunk* chunk = static_cast<Chunk*>(raw);
    chunk->next = chunks_;
    chunks_ = chunk;

    char* start = reinterpret_cast<char*>(chunk) + sizeof(Chunk);
    for (std::size_t i = 0; i < count; ++i)
    {
        FreeNode* node = reinterpret_cast<FreeNode*>(start + i * block_size_);
        node->next = free_list_;
        free_list_ = node;
    }

    total_ += count;
}

auto PoolAllocator::allocate() -> void*
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!free_list_)
    {
        std::size_t grow_count = total_ > 0 ? total_ : 64;
        grow(grow_count);
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

} // namespace atlas
