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
  // block_size must be at least sizeof(FreeNode) AND a multiple of alignment
  // so that every block in the slab starts at an aligned address.
  std::size_t min_size = block_size < sizeof(FreeNode) ? sizeof(FreeNode) : block_size;
  block_size_ = AlignUp(min_size, alignment_);
  Grow(initial_blocks);
}

PoolAllocator::~PoolAllocator() {
  // In debug builds, catch callers that destroy the pool while blocks are
  // still live.  This indicates a lifetime bug in the caller.
  assert(in_use_ == 0 && "PoolAllocator destroyed with blocks still in use");

  Chunk* chunk = chunks_;
  while (chunk) {
    Chunk* next = chunk->next;
    HeapFree(chunk);
    chunk = next;
  }
}

auto PoolAllocator::Grow(std::size_t count) -> bool {
  // Round the chunk header size up to alignment_ so the first block starts
  // at an aligned offset (malloc guarantees alignof(max_align_t) = 16 bytes
  // for the raw allocation, but sizeof(Chunk) = 8 which would misalign types
  // with alignof > 8 without this adjustment).
  std::size_t header_size = AlignUp(sizeof(Chunk), alignment_);
  std::size_t raw_size = header_size + block_size_ * count;
  // Slab itself goes through atlas::HeapAlloc — when the underlying
  // allocator is later swapped (mimalloc, jemalloc), pools follow
  // automatically. The slab is *not* reported to Tracy as a named
  // alloc; only the per-block carve below is, so the viewer's per-pool
  // memory line tracks user-visible blocks rather than slab geometry.
  void* raw = HeapAlloc(raw_size, alignment_);
  if (!raw) return false;  // OOM — caller decides what to do

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
    if (!Grow(grow_count)) return nullptr;  // OOM
  }

  FreeNode* node = free_list_;
  free_list_ = node->next;
  ++in_use_;

  // Named Tracy track per pool — the viewer shows TimerNode, BundleSlot,
  // etc. as separate memory streams. The block address here is what
  // user code receives, so the FREE_NAMED below pairs cleanly without
  // a wrapper layer the caller would need to know about.
  ATLAS_PROFILE_ALLOC_NAMED(node, block_size_, pool_name_);
  return static_cast<void*>(node);
}

void PoolAllocator::Deallocate(void* ptr) {
  if (!ptr) {
    return;
  }

  // Reported before unlinking — the pointer is already invalid as far
  // as the caller is concerned, and Tracy logs the free event with the
  // pool's named track. Pairing with the ALLOC_NAMED above is what
  // makes per-pool leak detection work in the viewer.
  ATLAS_PROFILE_FREE_NAMED(ptr, pool_name_);

  std::lock_guard<std::mutex> lock(mutex_);

  FreeNode* node = static_cast<FreeNode*>(ptr);
  node->next = free_list_;
  free_list_ = node;
  --in_use_;
}

}  // namespace atlas
