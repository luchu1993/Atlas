using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace Atlas.Client.Native
{
    // Pattern B per D0; see docs/spike_il2cpp_callback.md.
    public static unsafe class AtlasNetCallbackBridge
    {
        private static readonly ConcurrentDictionary<IntPtr, IAtlasNetEvents> CtxMap = new();

        public delegate void DisconnectFn(IntPtr ctx, int reason);
        public delegate void PlayerBaseCreateFn(IntPtr ctx, uint eid, ushort tid,
                                                byte* props, int len);
        public delegate void PlayerCellCreateFn(IntPtr ctx, uint spaceId,
                                                float px, float py, float pz,
                                                float dx, float dy, float dz,
                                                byte* props, int len);
        public delegate void ResetEntitiesFn(IntPtr ctx);
        public delegate void EntityEnterFn(IntPtr ctx, uint eid, ushort tid,
                                           float px, float py, float pz,
                                           float dx, float dy, float dz,
                                           byte* props, int len);
        public delegate void EntityLeaveFn(IntPtr ctx, uint eid);
        public delegate void EntityPositionFn(IntPtr ctx, uint eid,
                                              float px, float py, float pz,
                                              float dx, float dy, float dz,
                                              byte onGround);
        public delegate void EntityPropertyFn(IntPtr ctx, uint eid, byte scope,
                                              byte* delta, int len);
        public delegate void ForcedPositionFn(IntPtr ctx, uint eid,
                                              float px, float py, float pz,
                                              float dx, float dy, float dz);
        public delegate void RpcFn(IntPtr ctx, uint eid, uint rid, byte* payload, int len);

        // GC keep-alive; static lifetime keeps IL2CPP trampolines valid.
        private static readonly DisconnectFn        SDisconnect       = OnDisconnect;
        private static readonly PlayerBaseCreateFn  SPlayerBaseCreate = OnPlayerBaseCreate;
        private static readonly PlayerCellCreateFn  SPlayerCellCreate = OnPlayerCellCreate;
        private static readonly ResetEntitiesFn     SResetEntities    = OnResetEntities;
        private static readonly EntityEnterFn       SEntityEnter      = OnEntityEnter;
        private static readonly EntityLeaveFn       SEntityLeave      = OnEntityLeave;
        private static readonly EntityPositionFn    SEntityPosition   = OnEntityPosition;
        private static readonly EntityPropertyFn    SEntityProperty   = OnEntityProperty;
        private static readonly ForcedPositionFn    SForcedPosition   = OnForcedPosition;
        private static readonly RpcFn               SRpc              = OnRpc;

        public static void Register(IntPtr ctx, IAtlasNetEvents events)
        {
            if (ctx == IntPtr.Zero)  throw new ArgumentNullException(nameof(ctx));
            if (events == null)      throw new ArgumentNullException(nameof(events));
            CtxMap[ctx] = events;

            var table = new AtlasNetCallbacks
            {
                OnDisconnect       = Marshal.GetFunctionPointerForDelegate(SDisconnect),
                OnPlayerBaseCreate = Marshal.GetFunctionPointerForDelegate(SPlayerBaseCreate),
                OnPlayerCellCreate = Marshal.GetFunctionPointerForDelegate(SPlayerCellCreate),
                OnResetEntities    = Marshal.GetFunctionPointerForDelegate(SResetEntities),
                OnEntityEnter      = Marshal.GetFunctionPointerForDelegate(SEntityEnter),
                OnEntityLeave      = Marshal.GetFunctionPointerForDelegate(SEntityLeave),
                OnEntityPosition   = Marshal.GetFunctionPointerForDelegate(SEntityPosition),
                OnEntityProperty   = Marshal.GetFunctionPointerForDelegate(SEntityProperty),
                OnForcedPosition   = Marshal.GetFunctionPointerForDelegate(SForcedPosition),
                OnRpc              = Marshal.GetFunctionPointerForDelegate(SRpc),
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
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(PlayerBaseCreateFn))]
#endif
        private static void OnPlayerBaseCreate(IntPtr ctx, uint eid, ushort tid,
                                               byte* props, int len)
            => FromCtx(ctx)?.OnPlayerBaseCreate(eid, tid, MakeSpan(props, len));

#if UNITY_5_3_OR_NEWER
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(PlayerCellCreateFn))]
#endif
        private static void OnPlayerCellCreate(IntPtr ctx, uint spaceId,
                                               float px, float py, float pz,
                                               float dx, float dy, float dz,
                                               byte* props, int len)
            => FromCtx(ctx)?.OnPlayerCellCreate(spaceId, px, py, pz, dx, dy, dz,
                                                MakeSpan(props, len));

#if UNITY_5_3_OR_NEWER
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(ResetEntitiesFn))]
#endif
        private static void OnResetEntities(IntPtr ctx) => FromCtx(ctx)?.OnResetEntities();

#if UNITY_5_3_OR_NEWER
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(EntityEnterFn))]
#endif
        private static void OnEntityEnter(IntPtr ctx, uint eid, ushort tid,
                                          float px, float py, float pz,
                                          float dx, float dy, float dz,
                                          byte* props, int len)
            => FromCtx(ctx)?.OnEntityEnter(eid, tid, px, py, pz, dx, dy, dz,
                                           MakeSpan(props, len));

#if UNITY_5_3_OR_NEWER
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(EntityLeaveFn))]
#endif
        private static void OnEntityLeave(IntPtr ctx, uint eid)
            => FromCtx(ctx)?.OnEntityLeave(eid);

#if UNITY_5_3_OR_NEWER
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(EntityPositionFn))]
#endif
        private static void OnEntityPosition(IntPtr ctx, uint eid,
                                             float px, float py, float pz,
                                             float dx, float dy, float dz,
                                             byte onGround)
            => FromCtx(ctx)?.OnEntityPosition(eid, px, py, pz, dx, dy, dz, onGround != 0);

#if UNITY_5_3_OR_NEWER
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(EntityPropertyFn))]
#endif
        private static void OnEntityProperty(IntPtr ctx, uint eid, byte scope,
                                             byte* delta, int len)
            => FromCtx(ctx)?.OnEntityProperty(eid, scope, MakeSpan(delta, len));

#if UNITY_5_3_OR_NEWER
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(ForcedPositionFn))]
#endif
        private static void OnForcedPosition(IntPtr ctx, uint eid,
                                             float px, float py, float pz,
                                             float dx, float dy, float dz)
            => FromCtx(ctx)?.OnForcedPosition(eid, px, py, pz, dx, dy, dz);

#if UNITY_5_3_OR_NEWER
        [UnityEngine.AOT.MonoPInvokeCallback(typeof(RpcFn))]
#endif
        private static void OnRpc(IntPtr ctx, uint eid, uint rid, byte* payload, int len)
            => FromCtx(ctx)?.OnRpc(eid, rid, MakeSpan(payload, len));

        private static ReadOnlySpan<byte> MakeSpan(byte* p, int len)
            => p == null || len <= 0 ? default : new ReadOnlySpan<byte>(p, len);
    }
}
