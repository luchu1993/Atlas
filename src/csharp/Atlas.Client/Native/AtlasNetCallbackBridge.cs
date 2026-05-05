using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace Atlas.Client.Native;

// Single-callback bridge into net_client.dll. Hosts (Unity, Desktop, tests)
// register an IAtlasNetEvents implementation against an AtlasNetContext;
// every server-bound message routes through OnDeliver.
public static unsafe class AtlasNetCallbackBridge
{
    private static readonly ConcurrentDictionary<IntPtr, IAtlasNetEvents> CtxMap = new();

    public delegate void DisconnectFn(IntPtr ctx, int reason);
    public delegate void DeliverFn(IntPtr ctx, ushort msgId, byte* payload, int len);

    // GC keep-alive; static lifetime keeps IL2CPP trampolines valid.
    private static readonly DisconnectFn SDisconnect = OnDisconnect;
    private static readonly DeliverFn    SDeliver    = OnDeliver;

    public static void Register(IntPtr ctx, IAtlasNetEvents events)
    {
        if (ctx == IntPtr.Zero)  throw new ArgumentNullException(nameof(ctx));
        if (events == null)      throw new ArgumentNullException(nameof(events));
        CtxMap[ctx] = events;

        var table = new AtlasNetCallbacks
        {
            OnDisconnect = Marshal.GetFunctionPointerForDelegate(SDisconnect),
            OnDeliver    = Marshal.GetFunctionPointerForDelegate(SDeliver),
        };

        int rc = AtlasNetNative.AtlasNetSetCallbacks(ctx, ref table);
        if (rc != AtlasNetReturnCode.Ok)
        {
            CtxMap.TryRemove(ctx, out _);
            throw new InvalidOperationException($"AtlasNetSetCallbacks failed: rc={rc}");
        }
    }

    public static void Unregister(IntPtr ctx) => CtxMap.TryRemove(ctx, out _);

    private static IAtlasNetEvents? FromCtx(IntPtr ctx) =>
        CtxMap.TryGetValue(ctx, out var ev) ? ev : null;

#if UNITY_5_3_OR_NEWER
    [UnityEngine.AOT.MonoPInvokeCallback(typeof(DisconnectFn))]
#endif
    private static void OnDisconnect(IntPtr ctx, int reason)
        => FromCtx(ctx)?.OnDisconnect(reason);

#if UNITY_5_3_OR_NEWER
    [UnityEngine.AOT.MonoPInvokeCallback(typeof(DeliverFn))]
#endif
    private static void OnDeliver(IntPtr ctx, ushort msgId, byte* payload, int len)
        => FromCtx(ctx)?.OnDeliver(msgId, MakeSpan(payload, len));

    private static ReadOnlySpan<byte> MakeSpan(byte* p, int len)
        => p == null || len <= 0 ? default : new ReadOnlySpan<byte>(p, len);
}
