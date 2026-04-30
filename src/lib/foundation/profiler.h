#ifndef ATLAS_LIB_FOUNDATION_PROFILER_H_
#define ATLAS_LIB_FOUNDATION_PROFILER_H_

// ATLAS_PROFILE_ENABLED is the compile-time switch. When 0, every macro
// expands to a no-op at the preprocessor stage; no symbol reference,
// no link dependency exercised, no overhead beyond an empty expression.

#ifndef ATLAS_PROFILE_ENABLED
#define ATLAS_PROFILE_ENABLED 0
#endif

#if ATLAS_PROFILE_ENABLED

#include <tracy/Tracy.hpp>

#define ATLAS_PROFILE_FRAME(name) FrameMarkNamed(name)
#define ATLAS_PROFILE_FRAME_START(name) FrameMarkStart(name)
#define ATLAS_PROFILE_FRAME_END(name) FrameMarkEnd(name)

#define ATLAS_PROFILE_ZONE() ZoneScoped
#define ATLAS_PROFILE_ZONE_N(name) ZoneScopedN(name)
#define ATLAS_PROFILE_ZONE_C(name, color) ZoneScopedNC(name, color)

#define ATLAS_PROFILE_ZONE_TEXT(buf, len) ZoneText(buf, len)

#define ATLAS_PROFILE_PLOT(name, value) TracyPlot(name, value)

#define ATLAS_PROFILE_MESSAGE(buf, len) TracyMessage(buf, len)
#define ATLAS_PROFILE_MESSAGE_C(buf, len, c) TracyMessageC(buf, len, c)

// Stack capture costs a few microseconds per allocation; enable only in profile builds.
#define ATLAS_PROFILE_ALLOC(ptr, size) TracyAllocS(ptr, size, 16)
#define ATLAS_PROFILE_FREE(ptr) TracyFreeS(ptr, 16)
#define ATLAS_PROFILE_ALLOC_NAMED(p, s, n) TracyAllocNS(p, s, 16, n)
#define ATLAS_PROFILE_FREE_NAMED(p, n) TracyFreeNS(p, 16, n)

#define ATLAS_PROFILE_LOCKABLE(type, var) TracyLockable(type, var)
#define ATLAS_PROFILE_LOCK_MARK(var) LockMark(var)

#else  // ATLAS_PROFILE_ENABLED

#define ATLAS_PROFILE_FRAME(name) ((void)0)
#define ATLAS_PROFILE_FRAME_START(name) ((void)0)
#define ATLAS_PROFILE_FRAME_END(name) ((void)0)

#define ATLAS_PROFILE_ZONE() ((void)0)
#define ATLAS_PROFILE_ZONE_N(name) ((void)0)
#define ATLAS_PROFILE_ZONE_C(name, color) ((void)0)
#define ATLAS_PROFILE_ZONE_TEXT(buf, len) ((void)(buf), (void)(len))

#define ATLAS_PROFILE_PLOT(name, value) ((void)(value))

#define ATLAS_PROFILE_MESSAGE(buf, len) ((void)(buf), (void)(len))
#define ATLAS_PROFILE_MESSAGE_C(buf, len, c) ((void)(buf), (void)(len))

#define ATLAS_PROFILE_ALLOC(ptr, size) ((void)(ptr), (void)(size))
#define ATLAS_PROFILE_FREE(ptr) ((void)(ptr))
#define ATLAS_PROFILE_ALLOC_NAMED(p, s, n) ((void)(p), (void)(s))
#define ATLAS_PROFILE_FREE_NAMED(p, n) ((void)(p))

#define ATLAS_PROFILE_LOCKABLE(type, var) type var
#define ATLAS_PROFILE_LOCK_MARK(var) ((void)0)

#endif  // ATLAS_PROFILE_ENABLED

namespace atlas {

[[nodiscard]] auto ProfilerEnabled() noexcept -> bool;

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_PROFILER_H_
