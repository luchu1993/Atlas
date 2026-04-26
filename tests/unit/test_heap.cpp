#include <cstddef>
#include <cstdint>
#include <new>

#include <gtest/gtest.h>

#include "foundation/heap.h"

namespace {

TEST(Heap, AllocReturnsNonNullForReasonableSize) {
  void* p = atlas::HeapAlloc(64);
  ASSERT_NE(p, nullptr);
  atlas::HeapFree(p);
}

TEST(Heap, AllocRespectsRequestedAlignment) {
  // Pass an alignment a power-of-two larger than the platform default
  // so the test detects the case where HeapAlloc silently downgrades
  // to a non-aligned allocator. 64-byte alignment is a typical cache-
  // line constraint and still well within what aligned_alloc handles.
  constexpr std::size_t kAlign = 64;
  void* p = atlas::HeapAlloc(128, kAlign);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % kAlign, 0u);
  atlas::HeapFree(p);
}

TEST(Heap, FreeNullIsNoOp) {
  // Defensive — STL containers and smart pointers routinely free
  // nullptr on degenerate paths; HeapFree must not crash.
  atlas::HeapFree(nullptr);
}

TEST(Heap, OperatorNewRoutesThroughHeapFree) {
  // If the global operator new override is wired up correctly, memory
  // returned by `new` and memory returned by atlas::HeapAlloc share
  // the same underlying allocator (_aligned_malloc on Windows /
  // std::aligned_alloc on POSIX). Cross-freeing — `new` → HeapFree —
  // is therefore safe. Without the override, `new` would hit the CRT
  // default which on Windows uses a different free path, and this
  // test would crash under MSVC's debug heap. Passing here is direct
  // evidence the OBJECT-library propagation actually pulled heap.obj
  // into this test exe.
  int* p = new int(42);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 42);
  atlas::HeapFree(p);
}

TEST(Heap, OperatorDeleteAcceptsHeapAllocPointer) {
  // Symmetric to the previous test: HeapAlloc-allocated memory must
  // be freeable via the global operator delete. STL containers do
  // this routinely (custom allocators that wrap HeapAlloc would
  // hand pointers to ~vector which calls operator delete).
  void* raw = atlas::HeapAlloc(sizeof(int), alignof(int));
  ASSERT_NE(raw, nullptr);
  // Manual placement to keep the allocation paired with operator
  // delete rather than the typed delete that would expect a
  // constructed object's vtable.
  ::operator delete(raw);
}

}  // namespace
