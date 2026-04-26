#ifndef ATLAS_LIB_FOUNDATION_HEAP_H_
#define ATLAS_LIB_FOUNDATION_HEAP_H_

#include <cstddef>
#include <new>

namespace atlas {

// Atlas's primary heap interface. Today this delegates to
// std::aligned_alloc / _aligned_malloc; swapping in mimalloc or another
// allocator is a one-file rewrite of heap.cc — call sites stay put
// because every C++ `new` / `delete` (and every sized / aligned /
// nothrow variant) routes here through the global operator overrides
// defined alongside in heap.cc.
//
// Cross-DLL invariant.
//   Each Atlas binary (atlas_engine.dll, atlas_db_sqlite_plugin.dll,
//   atlas_db_xml_plugin.dll, every test exe) gets its own copy of
//   HeapAlloc / HeapFree via the atlas_heap OBJECT library. Pointers
//   routinely cross DLL boundaries — e.g. a Real entity allocated in
//   atlas_engine.dll may be freed inside a plugin DLL — so the
//   underlying allocator MUST expose a process-wide heap.
//   std::aligned_alloc satisfies this (one CRT, one process heap).
//   mimalloc's default override also satisfies it. Per-module or
//   per-thread arena modes (e.g. mimalloc's mi_heap_t) are OFF-LIMITS:
//   they would strand cross-DLL allocations on the wrong side at free
//   time. Any future swap of the body must check this.
//
// Both functions are noexcept and return / accept nullptr on OOM. The
// throwing behaviour required by `operator new` is provided by the
// override layer in heap.cc — this raw API is the OOM-returns-null
// shape a custom allocator most cleanly fits.

[[nodiscard]] auto HeapAlloc(std::size_t bytes,
                             std::size_t align = alignof(std::max_align_t)) noexcept -> void*;

void HeapFree(void* ptr) noexcept;

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_HEAP_H_
