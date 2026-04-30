#include "foundation/heap.h"

#include <cstdlib>
#include <new>

// Exactly one ATLAS_HEAP_<NAME> must be set by CMake.
#if defined(ATLAS_HEAP_MIMALLOC)
#include <mimalloc.h>
#elif defined(ATLAS_HEAP_STD)
#if defined(_WIN32)
#include <malloc.h>  // _aligned_malloc, _aligned_free
#endif
#else
#error \
    "No heap backend selected - cmake should define exactly one ATLAS_HEAP_<NAME> via ATLAS_HEAP_ALLOCATOR"
#endif

#include "foundation/profiler.h"

namespace atlas {

namespace {

#if ATLAS_PROFILE_ENABLED
// Prevent recursive Tracy allocation hooks if the profiler path allocates.
thread_local int g_alloc_depth = 0;

class AllocDepthGuard {
 public:
  AllocDepthGuard() noexcept { ++g_alloc_depth; }
  ~AllocDepthGuard() noexcept { --g_alloc_depth; }
  [[nodiscard]] auto IsTopLevel() const noexcept -> bool { return g_alloc_depth == 1; }
};
#endif

[[nodiscard]] auto RawAlloc(std::size_t bytes, std::size_t align) noexcept -> void* {
  // Backend APIs require at least max_align_t for default-aligned allocations.
  if (align < alignof(std::max_align_t)) align = alignof(std::max_align_t);
#if defined(ATLAS_HEAP_MIMALLOC)
  return mi_malloc_aligned(bytes, align);
#elif defined(_WIN32)
  return _aligned_malloc(bytes, align);
#else
  // POSIX aligned_alloc requires size to be a multiple of alignment.
  std::size_t rounded = (bytes + align - 1) & ~(align - 1);
  return std::aligned_alloc(align, rounded);
#endif
}

void RawFree(void* ptr) noexcept {
  if (!ptr) return;
#if defined(ATLAS_HEAP_MIMALLOC)
  mi_free(ptr);
#elif defined(_WIN32)
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

}  // namespace

auto HeapAlloc(std::size_t bytes, std::size_t align) noexcept -> void* {
#if ATLAS_PROFILE_ENABLED
  AllocDepthGuard guard;
  void* p = RawAlloc(bytes, align);
  if (p && guard.IsTopLevel()) {
    ATLAS_PROFILE_ALLOC(p, bytes);
  }
  return p;
#else
  return RawAlloc(bytes, align);
#endif
}

void HeapFree(void* ptr) noexcept {
  if (!ptr) return;
#if ATLAS_PROFILE_ENABLED
  AllocDepthGuard guard;
  if (guard.IsTopLevel()) {
    ATLAS_PROFILE_FREE(ptr);
  }
#endif
  RawFree(ptr);
}

}  // namespace atlas

// Keep the full replaceable-allocation surface on Atlas's heap backend.

namespace {

[[nodiscard]] auto AtlasOperatorNew(std::size_t size, std::size_t align) -> void* {
  void* p = atlas::HeapAlloc(size, align);
  if (!p) throw std::bad_alloc{};
  return p;
}

[[nodiscard]] auto AtlasOperatorNewNothrow(std::size_t size, std::size_t align) noexcept -> void* {
  return atlas::HeapAlloc(size, align);
}

}  // namespace

void* operator new(std::size_t size) {
  return AtlasOperatorNew(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size) {
  return AtlasOperatorNew(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t align) {
  return AtlasOperatorNew(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
  return AtlasOperatorNew(size, static_cast<std::size_t>(align));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return AtlasOperatorNewNothrow(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return AtlasOperatorNewNothrow(size, alignof(std::max_align_t));
}
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return AtlasOperatorNewNothrow(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
  return AtlasOperatorNewNothrow(size, static_cast<std::size_t>(align));
}

void operator delete(void* ptr) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete(void* ptr, std::align_val_t) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  atlas::HeapFree(ptr);
}

void operator delete[](void* ptr) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete[](void* ptr, std::size_t) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete[](void* ptr, std::align_val_t) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  atlas::HeapFree(ptr);
}
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  atlas::HeapFree(ptr);
}
