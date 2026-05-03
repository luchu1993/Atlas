using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

namespace Atlas.Core;

/// <summary>
/// Debug-only guard that verifies engine API calls happen on the main thread.
/// [Conditional("DEBUG")] ensures zero overhead in Release builds.
/// </summary>
internal static class ThreadGuard
{
    private static int _mainThreadId;
    public const int kMaxRpcDepth = 32;

    [ThreadStatic] private static int _rpcDepth;

    internal static void SetMainThread()
        => _mainThreadId = Environment.CurrentManagedThreadId;

    [Conditional("DEBUG")]
    internal static void EnsureMainThread(
        [CallerMemberName] string? caller = null)
    {
        if (Environment.CurrentManagedThreadId != _mainThreadId)
        {
            throw new InvalidOperationException(
                $"NativeApi.{caller}() must be called from the main thread. " +
                $"Current thread: {Environment.CurrentManagedThreadId}, " +
                $"main thread: {_mainThreadId}. " +
                $"Use 'await' to automatically return to the main thread.");
        }
    }

    // Caps RPC dispatch nesting so an in-process loopback path (test
    // doubles, future feature flags) can't recurse into a stack overflow.
    internal readonly struct RpcScope : IDisposable
    {
        public RpcScope(uint rpcId)
        {
            if (_rpcDepth >= kMaxRpcDepth)
                throw new InvalidOperationException(
                    $"RPC dispatch depth exceeded {kMaxRpcDepth}; possible recursion at rpcId=0x{rpcId:X8}");
            _rpcDepth++;
        }
        public void Dispose() => _rpcDepth--;
    }
}
