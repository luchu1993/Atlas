#ifndef ATLAS_LIB_FOUNDATION_HEAP_H_
#define ATLAS_LIB_FOUNDATION_HEAP_H_

#include <cstddef>
#include <new>

namespace atlas {

// Backend allocators must expose one process-wide heap because Atlas pointers
// can cross DLL/plugin boundaries before they are freed.

[[nodiscard]] auto HeapAlloc(std::size_t bytes,
                             std::size_t align = alignof(std::max_align_t)) noexcept -> void*;

void HeapFree(void* ptr) noexcept;

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_HEAP_H_
