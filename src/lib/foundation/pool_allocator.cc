#include "foundation/pool_allocator.h"

#include <cassert>
#include <cstring>

#include "foundation/heap.h"
#include "foundation/profiler.h"

namespace atlas {

namespace {
constexpr auto AlignUp(std::size_t n, std::size_t alignment) -> std::size_t {
  return (n + alignment - 1) & ~(alignment - 1);
}
}  // namespace

PoolAllocator::PoolAllocator(const char* pool_name, std::size_t block_size,
                             std::size_t initial_blocks, std::size_t alignment)
    : pool_name_(pool_name),
      alignment_(alignment < alignof(FreeNode) ? alignof(FreeNode) : alignment) {
  std::size_t min_size = block_size < sizeof(FreeNode) ? sizeof(FreeNode) : block_size;
  block_size_ = AlignUp(min_size, alignment_);
  Grow(initial_blocks);
}

PoolAllocator::~PoolAllocator() {
  assert(in_use_ == 0 && "PoolAllocator destroyed with blocks still in use");

  Chunk* chunk = chunks_;
  while (chunk) {
    Chunk* next = chunk->next;
    HeapFree(chunk);
    chunk = next;
  }
}

auto PoolAllocator::Grow(std::size_t count) -> bool {
  std::size_t header_size = AlignUp(sizeof(Chunk), alignment_);
  std::size_t raw_size = header_size + block_size_ * count;
  void* raw = HeapAlloc(raw_size, alignment_);
  if (!raw) return false;

  Chunk* chunk = static_cast<Chunk*>(raw);
  chunk->next = chunks_;
  chunks_ = chunk;

  char* start = reinterpret_cast<char*>(chunk) + header_size;
  for (std::size_t i = 0; i < count; ++i) {
    FreeNode* node = reinterpret_cast<FreeNode*>(start + i * block_size_);
    node->next = free_list_;
    free_list_ = node;
  }

  total_ += count;
  return true;
}

auto PoolAllocator::Allocate() -> void* {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!free_list_) {
    std::size_t grow_count = total_ > 0 ? total_ : 64;
    if (!Grow(grow_count)) return nullptr;
  }

  FreeNode* node = free_list_;
  free_list_ = node->next;
  ++in_use_;

  ATLAS_PROFILE_ALLOC_NAMED(node, block_size_, pool_name_);
  return static_cast<void*>(node);
}

void PoolAllocator::Deallocate(void* ptr) {
  if (!ptr) {
    return;
  }

  ATLAS_PROFILE_FREE_NAMED(ptr, pool_name_);

  std::lock_guard<std::mutex> lock(mutex_);

  FreeNode* node = static_cast<FreeNode*>(ptr);
  node->next = free_list_;
  free_list_ = node;
  --in_use_;
}

}  // namespace atlas
