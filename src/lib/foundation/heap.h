#ifndef ATLAS_LIB_FOUNDATION_HEAP_H_
#define ATLAS_LIB_FOUNDATION_HEAP_H_

#include <cstddef>
#include <new>

namespace atlas {

// Atlas's primary heap interface. The implementation in heap.cc
// dispatches to one of several allocator backends selected at
// configure time via ATLAS_HEAP_ALLOCATOR (std, mimalloc, …). Call
// sites stay backend-agnostic because every C++ `new` / `delete` —
// and every sized / aligned / nothrow variant — routes here through
// the global operator overrides defined alongside in heap.cc.
//
// Cross-DLL invariant.
//   Each Atlas binary (atlas_engine.dll, atlas_db_sqlite_plugin.dll,
//   atlas_db_xml_plugin.dll, every test exe) gets its own copy of
//   HeapAlloc / HeapFree via the atlas_heap OBJECT library. Pointers
//   routinely cross DLL boundaries — e.g. a Real entity allocated in
//   atlas_engine.dll may be freed inside a plugin DLL — so the
//   underlying allocator MUST expose a process-wide heap.
//
//   * "std" backend: platform CRT (one CRT, one heap under /MD on
//     MSVC and under libc on Linux). Cross-DLL safe by construction.
//   * "mimalloc" backend: shared-library build (MI_BUILD_SHARED), one
//     mi_heap instance per process. Cross-DLL safe.
//
//   Per-module or per-thread arena modes (e.g. mimalloc's mi_heap_t,
//   tcmalloc's per-thread caches with explicit ownership) are
//   OFF-LIMITS — they would strand cross-DLL allocations on the wrong
//   side at free time. Any new backend added under
//   ATLAS_HEAP_ALLOCATOR must verify this property before going in.
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
