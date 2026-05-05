using System;
using System.Runtime.CompilerServices;

namespace Atlas.Diagnostics
{
    public static class Log
    {
        private static ILogBackend backend_ = NullLogBackend.Instance;

        public static ILogBackend Backend
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => backend_;
        }

        // Exactly one non-null backend per process — reject re-install so a
        // misordered bootstrap surfaces instead of silently winning.
        public static bool SetBackend(ILogBackend backend)
        {
            if (backend == null) throw new ArgumentNullException(nameof(backend));
            var current = backend_;
            if (!ReferenceEquals(current, NullLogBackend.Instance) &&
                !ReferenceEquals(current, backend))
            {
                return false;
            }
            backend_ = backend;
            return true;
        }

        public static void ResetBackend() => backend_ = NullLogBackend.Instance;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Trace(string message) => backend_.Log(0, message);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Debug(string message) => backend_.Log(1, message);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Info(string message) => backend_.Log(2, message);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Warning(string message) => backend_.Log(3, message);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Error(string message) => backend_.Log(4, message);

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Critical(string message) => backend_.Log(5, message);
    }
}
