using System;
using System.Runtime.InteropServices;

namespace Atlas.Runtime.Diagnostics
{
    /// <summary>
    /// Minimal P/Invoke bindings for Tracy 0.13.x's TracyC.h surface — just
    /// the entry points <see cref="TracyProfilerBackend"/> needs.
    /// Library name <c>TracyClient</c> matches the CMake target built as a
    /// SHARED library in cmake/Dependencies.cmake; both the C++ link path
    /// and these P/Invoke calls resolve to the same in-process Tracy
    /// instance, which is what merges native and managed zones into one
    /// trace timeline.
    /// </summary>
    /// <remarks>
    /// This file deliberately re-implements what packages like Tracy-NET
    /// would otherwise provide, because the available NuGet packages in
    /// April 2026 either pin the Tracy native version too low (Tracy-CSharp
    /// 0.11.1) or require net10.0 (Tracy-NET 0.13.x) — Atlas.Runtime is
    /// net9.0 and the native layer just bumped to Tracy 0.13.1, so neither
    /// fits. Ownership of the bindings keeps the version-pin policy
    /// trivial: bump the Tracy native tag in cmake/Dependencies.cmake,
    /// then re-check the function list below against the new TracyC.h.
    /// </remarks>
    internal static class TracyNative
    {
        private const string LibraryName = "TracyClient";

        /// <summary>
        /// Tracy's zone context. Layout matches TracyC.h's
        /// <c>___tracy_c_zone_context</c> exactly so the struct can be
        /// returned by value across the P/Invoke boundary and bit-cast to
        /// IntPtr at the IProfilerBackend layer (both are 8 bytes on x64).
        /// </summary>
        [StructLayout(LayoutKind.Sequential)]
        public struct TracyCZoneCtx
        {
            public uint Id;
            public int Active;
        }

        // Source-location allocation. Returns an opaque uint64 token that
        // ___tracy_emit_zone_begin_alloc consumes. Tracy copies the bytes
        // from the supplied buffers into its own arena, so the caller's
        // buffers can be freed immediately after the call returns.
        [DllImport(LibraryName, EntryPoint = "___tracy_alloc_srcloc_name", CallingConvention = CallingConvention.Cdecl)]
        public static extern unsafe ulong AllocSrclocName(
            uint line, byte* source, nuint sourceSz,
            byte* function, nuint functionSz,
            byte* name, nuint nameSz, uint color);

        [DllImport(LibraryName, EntryPoint = "___tracy_emit_zone_begin_alloc", CallingConvention = CallingConvention.Cdecl)]
        public static extern TracyCZoneCtx EmitZoneBeginAlloc(ulong srcloc, int active);

        [DllImport(LibraryName, EntryPoint = "___tracy_emit_zone_end", CallingConvention = CallingConvention.Cdecl)]
        public static extern void EmitZoneEnd(TracyCZoneCtx ctx);

        [DllImport(LibraryName, EntryPoint = "___tracy_emit_message", CallingConvention = CallingConvention.Cdecl)]
        public static extern unsafe void EmitMessage(byte* txt, nuint size, int callstackDepth);

        // Frame mark accepts a const char* whose pointer identity Tracy uses
        // to key the frame group — so the buffer must outlive the program,
        // not the call. The managed wrapper caches a marshalled copy per
        // distinct frame name and re-passes that same pointer on every tick.
        [DllImport(LibraryName, EntryPoint = "___tracy_emit_frame_mark", CallingConvention = CallingConvention.Cdecl)]
        public static extern void EmitFrameMark(IntPtr name);

        // Same pointer-identity convention as frame names — the plot label
        // pointer must be stable for the life of the process.
        [DllImport(LibraryName, EntryPoint = "___tracy_emit_plot", CallingConvention = CallingConvention.Cdecl)]
        public static extern void EmitPlot(IntPtr name, double val);

        [DllImport(LibraryName, EntryPoint = "___tracy_connected", CallingConvention = CallingConvention.Cdecl)]
        public static extern int Connected();

        [DllImport(LibraryName, EntryPoint = "___tracy_set_thread_name", CallingConvention = CallingConvention.Cdecl)]
        public static extern unsafe void SetThreadName(byte* name);
    }
}
