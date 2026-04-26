using System;
using System.Runtime.CompilerServices;

namespace Atlas.Diagnostics
{
    /// <summary>
    /// Atlas's managed profiler facade. Mirror of the C++ <c>ATLAS_PROFILE_*</c>
    /// macro surface in <c>foundation/profiler.h</c> — the call sites use this
    /// type and never reach into Tracy / Unity Profiler directly, so swapping
    /// the backend is a single registration call rather than a sweep across
    /// the codebase.
    /// </summary>
    /// <remarks>
    /// Methods are aggressively inlined and the backend slot is a static
    /// field, so the steady-state cost of an idle backend (NullProfilerBackend)
    /// is one volatile read + one virtual call the JIT can devirtualise once
    /// the backend stabilises. On a real backend (Tracy / Unity) the cost is
    /// dominated by the backend itself, not the facade.
    /// </remarks>
    public static class Profiler
    {
        private static IProfilerBackend backend_ = NullProfilerBackend.Instance;

        public static IProfilerBackend Backend
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => backend_;
        }

        /// <summary>
        /// Installs <paramref name="backend"/> as the active profiler sink.
        /// Calling this twice with different backends in the same process is
        /// rejected — the design is "exactly one backend per process" so
        /// Tracy and Unity Profiler don't fight over the same call sites.
        /// Returns <c>true</c> if the backend was installed.
        /// </summary>
        public static bool SetBackend(IProfilerBackend backend)
        {
            if (backend == null) throw new ArgumentNullException(nameof(backend));
            // Allow the null backend to be re-installed (idempotent reset)
            // and allow swapping FROM the null backend to a real one. Reject
            // installing a second non-null backend — that's almost always a
            // bootstrap bug, and silently winning either way would hide it.
            var current = backend_;
            if (!ReferenceEquals(current, NullProfilerBackend.Instance) &&
                !ReferenceEquals(current, backend))
            {
                return false;
            }
            backend_ = backend;
            return true;
        }

        /// <summary>
        /// Reverts to the no-op backend. Test-only entry point — production
        /// code installs a backend at boot and never tears it down.
        /// </summary>
        public static void ResetBackend()
        {
            backend_ = NullProfilerBackend.Instance;
        }

        /// <summary>
        /// Opens a scoped zone named <paramref name="name"/>. The returned
        /// struct must be disposed (via <c>using</c>) to close the zone —
        /// failing to dispose leaves the zone open until the next frame
        /// boundary, which the viewer renders as one absurdly long zone
        /// covering everything that followed.
        /// </summary>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static ProfilerZone Zone([CallerMemberName] string name = "", uint color = 0)
        {
            var token = backend_.BeginZone(name, color);
            return new ProfilerZone(backend_, token);
        }

        /// <summary>
        /// Opens a scoped zone with an explicit literal name. Prefer this
        /// over the auto-named overload on hot paths — the literal can be
        /// interned by the backend (Tracy hashes by pointer, Unity caches
        /// ProfilerMarker by name) which avoids per-call allocation.
        /// </summary>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static ProfilerZone ZoneN(string name, uint color = 0)
        {
            var token = backend_.BeginZone(name, color);
            return new ProfilerZone(backend_, token);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Plot(string name, double value) => backend_.Plot(name, value);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Plot(string name, int value) => backend_.Plot(name, value);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Message(string text) => backend_.Message(text);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void FrameMark(string name) => backend_.FrameMark(name);
    }

    /// <summary>
    /// RAII handle returned by <see cref="Profiler.Zone(string, uint)"/>. A
    /// struct rather than a class so the zone open/close pair allocates
    /// nothing on the managed heap — critical at hot-path call rates.
    /// </summary>
    public readonly struct ProfilerZone : IDisposable
    {
        private readonly IProfilerBackend backend_;
        private readonly IntPtr token_;

        internal ProfilerZone(IProfilerBackend backend, IntPtr token)
        {
            backend_ = backend;
            token_ = token;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Dispose()
        {
            // backend_ is null only for `default(ProfilerZone)` values which
            // never came from BeginZone — defensively skip rather than NRE.
            backend_?.EndZone(token_);
        }
    }
}
