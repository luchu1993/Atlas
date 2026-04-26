using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

using Atlas.Diagnostics;

namespace Atlas.Runtime.Diagnostics
{
    /// <summary>
    /// <see cref="IProfilerBackend"/> implementation that pipes Atlas zones
    /// into the in-process Tracy client. Installed by ScriptHost during
    /// server boot (after CoreCLR is up, so the same Tracy instance the
    /// native side is already using is what we attach to).
    /// </summary>
    /// <remarks>
    /// Frame and plot names are cached as native UTF-8 buffers because
    /// Tracy keys those streams by pointer identity — passing a fresh
    /// marshal of "DungeonTick" each tick would create one new frame
    /// stream per tick. The cache leaks intentionally; both name spaces
    /// are small and bounded for a server's lifetime.
    /// </remarks>
    public sealed class TracyProfilerBackend : IProfilerBackend
    {
        private readonly ConcurrentDictionary<string, IntPtr> frameNames_ = new ConcurrentDictionary<string, IntPtr>();
        private readonly ConcurrentDictionary<string, IntPtr> plotNames_ = new ConcurrentDictionary<string, IntPtr>();

        public unsafe IntPtr BeginZone(string name, uint color)
        {
            // Tracy's TracyAllocSrclocName copies the byte ranges into its
            // own queue, so stack-allocated UTF-8 buffers are safe — no
            // managed allocation per call on the hot path.
            var nameBytes = name ?? string.Empty;
            int byteCount = Encoding.UTF8.GetByteCount(nameBytes);
            Span<byte> buf = byteCount <= 256 ? stackalloc byte[byteCount] : new byte[byteCount];
            Encoding.UTF8.GetBytes(nameBytes, buf);

            ulong srcloc;
            fixed (byte* p = buf)
            {
                // Pass the same buffer for `function` and `name` so the
                // viewer renders something useful. File path stays empty —
                // Atlas calls don't capture file/line at the IProfilerBackend
                // boundary, and adding optional caller-info parameters
                // would force every call site through a different overload.
                srcloc = TracyNative.AllocSrclocName(
                    line: 0,
                    source: null, sourceSz: 0,
                    function: p, functionSz: (nuint)byteCount,
                    name: p, nameSz: (nuint)byteCount,
                    color: color);
            }
            var ctx = TracyNative.EmitZoneBeginAlloc(srcloc, active: 1);

            // TracyCZoneCtx is { uint, int } = 8 bytes; IntPtr is 8 bytes
            // on x64. Bit-cast rather than allocate so zone open/close stays
            // allocation-free at hot-path call rates. The runtime check
            // catches the impossible 32-bit case rather than silently
            // truncating active=true into garbage.
            Debug.Assert(sizeof(TracyNative.TracyCZoneCtx) == sizeof(IntPtr),
                "TracyCZoneCtx and IntPtr must be the same size for token bit-cast");
            return *(IntPtr*)&ctx;
        }

        public unsafe void EndZone(IntPtr token)
        {
            var ctx = *(TracyNative.TracyCZoneCtx*)&token;
            TracyNative.EmitZoneEnd(ctx);
        }

        public void Plot(string name, double value)
        {
            var ptr = GetCachedNamePtr(plotNames_, name);
            TracyNative.EmitPlot(ptr, value);
        }

        public unsafe void Message(string text)
        {
            if (text == null) return;
            int byteCount = Encoding.UTF8.GetByteCount(text);
            Span<byte> buf = byteCount <= 256 ? stackalloc byte[byteCount] : new byte[byteCount];
            Encoding.UTF8.GetBytes(text, buf);
            fixed (byte* p = buf)
            {
                TracyNative.EmitMessage(p, (nuint)byteCount, callstackDepth: 0);
            }
        }

        public void FrameMark(string name)
        {
            var ptr = GetCachedNamePtr(frameNames_, name);
            TracyNative.EmitFrameMark(ptr);
        }

        public bool IsConnected()
        {
            return TracyNative.Connected() != 0;
        }

        public unsafe void SetThreadName(string name)
        {
            if (string.IsNullOrEmpty(name)) return;
            int byteCount = Encoding.UTF8.GetByteCount(name);
            // +1 for null terminator: SetThreadName takes a C string, no length.
            Span<byte> buf = byteCount + 1 <= 256 ? stackalloc byte[byteCount + 1] : new byte[byteCount + 1];
            Encoding.UTF8.GetBytes(name, buf);
            buf[byteCount] = 0;
            fixed (byte* p = buf)
            {
                TracyNative.SetThreadName(p);
            }
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static IntPtr GetCachedNamePtr(ConcurrentDictionary<string, IntPtr> cache, string name)
        {
            if (cache.TryGetValue(name, out var existing)) return existing;
            // Marshal a stable, null-terminated C string. The buffer is
            // intentionally never freed — Tracy keys streams by pointer
            // identity, and freeing would corrupt the trace history.
            // Bounded by the number of distinct frame/plot names a server
            // emits, which is O(call sites) not O(ticks).
            var ptr = Marshal.StringToHGlobalAnsi(name);
            return cache.GetOrAdd(name, ptr);
        }
    }
}
