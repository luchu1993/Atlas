#include "foundation/heap.h"

#include <cstdlib>
#include <new>

// Backend selection. Exactly one ATLAS_HEAP_<NAME> must be set; the
// CMake glue in cmake/AtlasCompilerOptions.cmake is the single source
// of truth for which one. The hard #error catches any future build
// path that forgets to plumb the define so a silent fallthrough never
// fragments the heap policy across translation units.
#if defined(ATLAS_HEAP_MIMALLOC)
#include <mimalloc.h>
#elif defined(ATLAS_HEAP_STD)
#if defined(_WIN32)
#include <malloc.h>  // _aligned_malloc, _aligned_free
#endif
#else
#error \
    "No heap backend selected — cmake should define exactly one ATLAS_HEAP_<NAME> via ATLAS_HEAP_ALLOCATOR"
#endif

#include "foundation/profiler.h"

namespace atlas {

namespace {

// Per-thread re-entry depth counter. Tracy's own internals are routed
// through TracyClient.dll's separate CRT and don't come back through
// this override, so the common case is depth never exceeds 1. The
// guard is a defence against pathological paths — for example, lazy
// per-thread Tracy queue init that hits operator new on a code path
// we didn't anticipate. Without it, that single misroute would loop
// forever; with it, the inner alloc bypasses the Tracy hook and
// terminates cleanly.
thread_local int g_alloc_depth = 0;

class AllocDepthGuard {
 public:
  AllocDepthGuard() noexcept { ++g_alloc_depth; }
  ~AllocDepthGuard() noexcept { --g_alloc_depth; }
  [[nodiscard]] auto IsTopLevel() const noexcept -> bool { return g_alloc_depth == 1; }
};

[[nodiscard]] auto RawAlloc(std::size_t bytes, std::size_t align) noexcept -> void* {
  // Aligned-alloc preconditions vary by platform and backend but
  // always require alignment to be a power of two and at least
  // sizeof(void*). Atlas call sites pass alignof(T) which the
  // language already guarantees is a power of two, so the only real
  // coercion is the lower bound. Default-aligned allocations (the
  // bulk of operator new traffic) still need a pow-2 ≥ sizeof(void*);
  // clamp to max_align_t.
  if (align < alignof(std::max_align_t)) align = alignof(std::max_align_t);
#if defined(ATLAS_HEAP_MIMALLOC)
  // mi_malloc_aligned handles the size↔align relationship internally;
  // mi_free pairs with both aligned and unaligned allocations, so
  // RawFree below is the single counterpart.
  return mi_malloc_aligned(bytes, align);
#elif defined(_WIN32)
  return _aligned_malloc(bytes, align);
#else
  // POSIX std::aligned_alloc requires `bytes` to be a multiple of
  // `align`; round up. Returning a slightly larger block than asked
  // for is harmless — operator delete passes only the pointer back.
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
  AllocDepthGuard guard;
  void* p = RawAlloc(bytes, align);
  if (p && guard.IsTopLevel()) {
    ATLAS_PROFILE_ALLOC(p, bytes);
  }
  return p;
}

void HeapFree(void* ptr) noexcept {
  if (!ptr) return;
  AllocDepthGuard guard;
  if (guard.IsTopLevel()) {
    ATLAS_PROFILE_FREE(ptr);
  }
  RawFree(ptr);
}

}  // namespace atlas

// ============================================================================
// Global operator new / delete overrides
// ============================================================================
//
// The full C++17 replaceable-allocation surface, all forwarding to
// atlas::HeapAlloc / atlas::HeapFree. Coverage matters because the linker
// resolves `new` and `delete` per call-site, not as a single decision: a
// missing override means that one variant silently hits the CRT default
// instead, fragmenting allocator state across two backends.
//
// 8 `new` variants (throwing × array × aligned × nothrow combinations) and
// 12 `delete` variants (the four `delete` axes plus C++14 sized delete,
// crossed with array / aligned / nothrow) — 20 entry points total.
//
// Only C++ operator new is intercepted. C-style malloc/free remain the
// platform CRT's; that's intentional, because Tracy itself uses malloc
// internally and we don't want to recurse through it.

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

// ── Throwing scalar / array ────────────────────────────────────────────────
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

// ── Nothrow scalar / array ─────────────────────────────────────────────────
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

// ── Scalar delete (6 variants) ─────────────────────────────────────────────
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

// ── Array delete (6 variants) ──────────────────────────────────────────────
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
