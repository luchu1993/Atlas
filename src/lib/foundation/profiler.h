#ifndef ATLAS_LIB_FOUNDATION_PROFILER_H_
#define ATLAS_LIB_FOUNDATION_PROFILER_H_

// Atlas-wide profiler abstraction.
//
// Every Atlas source file uses ATLAS_PROFILE_* macros below. No file
// outside this header and its companion .cc may include the underlying
// backend's headers. Replacing the backend (Tracy → Optick / Perfetto /
// internal) is intentionally a single-file rewrite of this surface; the
// ~thousands of call sites do not move.
//
// ATLAS_PROFILE_ENABLED is the compile-time switch. When 0, every macro
// expands to a no-op at the preprocessor stage — no symbol reference,
// no link dependency exercised, no overhead beyond an empty expression.

#ifndef ATLAS_PROFILE_ENABLED
#define ATLAS_PROFILE_ENABLED 0
#endif

#if ATLAS_PROFILE_ENABLED

// Tracy is the current backend. It owns the on-the-wire format,
// timestamping, and the viewer protocol. Anything Tracy-shaped that
// leaks into Atlas code outside this file violates the abstraction.
#include <tracy/Tracy.hpp>

// ── Frame markers ──────────────────────────────────────────────────
// Emitted once per top-level tick boundary. Distinct names let the
// viewer separate logical frames (e.g. OpenWorldTick vs DungeonTick)
// from any other periodic events.
#define ATLAS_PROFILE_FRAME(name) FrameMarkNamed(name)
#define ATLAS_PROFILE_FRAME_START(name) FrameMarkStart(name)
#define ATLAS_PROFILE_FRAME_END(name) FrameMarkEnd(name)

// ── Scoped zones ───────────────────────────────────────────────────
// RAII: the zone closes when the enclosing block exits. The auto form
// captures the surrounding function name; the _N form takes a
// compile-time literal so the viewer does not interleave zones whose
// names share a prefix; _C overrides the colour.
#define ATLAS_PROFILE_ZONE() ZoneScoped
#define ATLAS_PROFILE_ZONE_N(name) ZoneScopedN(name)
#define ATLAS_PROFILE_ZONE_C(name, color) ZoneScopedNC(name, color)

// Attach a dynamic string fragment to the *current* zone — useful for
// per-call context (e.g. entity id, message id) that cannot be encoded
// in a compile-time name.
#define ATLAS_PROFILE_ZONE_TEXT(buf, len) ZoneText(buf, len)

// ── Plots ──────────────────────────────────────────────────────────
// Scalar time series. Use for queue depths, entity counts, bandwidth
// rates — anything that benefits from a continuous chart aligned to
// the frame timeline.
#define ATLAS_PROFILE_PLOT(name, value) TracyPlot(name, value)

// ── Free-form messages ─────────────────────────────────────────────
// Single-line annotations bound to the timeline. The most common use
// is propagating the OpenTelemetry traceparent across a network hop so
// the viewer can cross-reference the distributed span.
#define ATLAS_PROFILE_MESSAGE(buf, len) TracyMessage(buf, len)
#define ATLAS_PROFILE_MESSAGE_C(buf, len, c) TracyMessageC(buf, len, c)

// ── Allocator hooks ────────────────────────────────────────────────
// Wrap operator new / pool acquire / pool release. The _NAMED variants
// route per-pool so the viewer can attribute heap pressure by source.
#define ATLAS_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define ATLAS_PROFILE_FREE(ptr) TracyFree(ptr)
#define ATLAS_PROFILE_ALLOC_NAMED(p, s, n) TracyAllocN(p, s, n)
#define ATLAS_PROFILE_FREE_NAMED(p, n) TracyFreeN(p, n)

// ── Lock contention ────────────────────────────────────────────────
// Replace `std::mutex m;` with `ATLAS_PROFILE_LOCKABLE(std::mutex, m);`
// to participate in the contention timeline. ATLAS_PROFILE_LOCK_MARK
// emits a synchronisation event without claiming the mutex itself.
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

// In disabled mode the lockable stays a plain mutex declaration so
// existing code keeps working unchanged; the lock-mark is dropped.
#define ATLAS_PROFILE_LOCKABLE(type, var) type var
#define ATLAS_PROFILE_LOCK_MARK(var) ((void)0)

#endif  // ATLAS_PROFILE_ENABLED

namespace atlas {

// Reports the compile-time state of the profiler abstraction. Useful
// for log banners and for guarding profiler-specific test paths
// without leaking the macro into call sites.
[[nodiscard]] auto ProfilerEnabled() noexcept -> bool;

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_PROFILER_H_
