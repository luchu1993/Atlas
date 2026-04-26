#include <cstdint>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "foundation/profiler.h"

namespace {

TEST(Profiler, EnabledReportsCompileTimeState) {
#if ATLAS_PROFILE_ENABLED
  EXPECT_TRUE(atlas::ProfilerEnabled());
#else
  EXPECT_FALSE(atlas::ProfilerEnabled());
#endif
}

TEST(Profiler, FrameMarkersCompile) {
  ATLAS_PROFILE_FRAME("UnitTest");
  ATLAS_PROFILE_FRAME_START("UnitTestPaired");
  ATLAS_PROFILE_FRAME_END("UnitTestPaired");
}

TEST(Profiler, ZoneMacrosCompile) {
  ATLAS_PROFILE_ZONE();
  {
    ATLAS_PROFILE_ZONE_N("Named");
    int volatile sum = 0;
    for (int i = 0; i < 10; ++i) sum += i;
    EXPECT_EQ(sum, 45);
  }
  {
    ATLAS_PROFILE_ZONE_C("Coloured", 0xff0000);
    const char text[] = "context";
    ATLAS_PROFILE_ZONE_TEXT(text, sizeof(text) - 1);
  }
}

TEST(Profiler, PlotsAndMessagesCompile) {
  ATLAS_PROFILE_PLOT("UnitTestPlot", 1.5);
  const char msg[] = "trace=abcdef";
  ATLAS_PROFILE_MESSAGE(msg, sizeof(msg) - 1);
  ATLAS_PROFILE_MESSAGE_C(msg, sizeof(msg) - 1, 0x00ff00);
}

TEST(Profiler, AllocatorHooksCompile) {
  // Allocator macros should accept any pointer-sized value. They must
  // never dereference what the caller hands them, so a fake address
  // is safe and keeps the test free of heap traffic.
  void* fake_ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xCAFEBABE));
  ATLAS_PROFILE_ALLOC(fake_ptr, 64);
  ATLAS_PROFILE_FREE(fake_ptr);
  ATLAS_PROFILE_ALLOC_NAMED(fake_ptr, 64, "UnitTestPool");
  ATLAS_PROFILE_FREE_NAMED(fake_ptr, "UnitTestPool");
}

TEST(Profiler, LockableExpandsToUsableMutex) {
  // The macro must produce a valid declaration in both modes. In the
  // disabled build it degrades to a plain std::mutex; in the enabled
  // build Tracy wraps it. Either way, lock/unlock must work.
  ATLAS_PROFILE_LOCKABLE(std::mutex, m);
  m.lock();
  ATLAS_PROFILE_LOCK_MARK(m);
  m.unlock();
}

}  // namespace
