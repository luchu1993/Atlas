using System;
using Atlas.Client;
using Atlas.Client.Native;
using Atlas.DataTypes;
using UnityEngine;

namespace Atlas.Client.Unity
{
    public sealed class AtlasNetworkManager : MonoBehaviour, IAtlasNetEvents
    {
        [SerializeField] private string loginappHost = "127.0.0.1";
        [SerializeField] private ushort loginappPort = 20018;

        public event Action<AtlasLoginStatus, string?>? LoginFinished;
        public event Action<bool, uint, ushort, string?>? AuthFinished;
        public event Action<int>? Disconnected;
        public event Action<uint, ushort, byte[]>? PlayerBaseCreated;
        public event Action<uint, Vector3, Vector3, byte[]>? PlayerCellCreated;
        public event Action? EntitiesReset;
        public event Action<uint, ushort, Vector3, Vector3, byte[]>? EntityEntered;
        public event Action<uint>? EntityLeft;
        public event Action<uint, Vector3, Vector3, bool>? EntityPositionUpdated;
        public event Action<uint, byte, byte[]>? EntityPropertyUpdated;
        public event Action<uint, Vector3, Vector3>? EntityForcedPosition;
        public event Action<uint, uint, byte[]>? Rpc;

        public AtlasNetState State =>
            _ctx == IntPtr.Zero ? AtlasNetState.Disconnected : AtlasNetNative.AtlasNetGetState(_ctx);

        private IntPtr _ctx;
        private AtlasNetNative.LoginResultDelegate? _loginCallback;
        private AtlasNetNative.AuthResultDelegate? _authCallback;

        private void Awake()
        {
            _ctx = AtlasNetNative.Create();
            AtlasNetCallbackBridge.Register(_ctx, this);
        }

        private void Update()
        {
            if (_ctx != IntPtr.Zero) AtlasNetNative.AtlasNetPoll(_ctx);
        }

        private void OnDestroy()
        {
            if (_ctx == IntPtr.Zero) return;
            AtlasNetCallbackBridge.Unregister(_ctx);
            AtlasNetNative.AtlasNetDestroy(_ctx);
            _ctx = IntPtr.Zero;
        }

        public int Login(string username, string passwordHash)
        {
            if (_ctx == IntPtr.Zero) return AtlasNetReturnCode.ErrInval;
            _loginCallback = OnLoginNative;
            IntPtr cb = System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(_loginCallback);
            return AtlasNetNative.AtlasNetLogin(_ctx, loginappHost, loginappPort,
                                                username, passwordHash, cb, IntPtr.Zero);
        }

        public int Authenticate()
        {
            if (_ctx == IntPtr.Zero) return AtlasNetReturnCode.ErrInval;
            _authCallback = OnAuthNative;
            IntPtr cb = System.Runtime.InteropServices.Marshal.GetFunctionPointerForDelegate(_authCallback);
            return AtlasNetNative.AtlasNetAuthenticate(_ctx, cb, IntPtr.Zero);
        }

        public int Logout()
            => _ctx == IntPtr.Zero
                ? AtlasNetReturnCode.ErrInval
                : AtlasNetNative.AtlasNetDisconnect(_ctx, AtlasDisconnectReason.Logout);

        public unsafe int SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
        {
            if (_ctx == IntPtr.Zero) return AtlasNetReturnCode.ErrInval;
            fixed (byte* p = payload)
                return AtlasNetNative.AtlasNetSendBaseRpc(_ctx, entityId, rpcId, p, payload.Length);
        }

        public unsafe int SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
        {
            if (_ctx == IntPtr.Zero) return AtlasNetReturnCode.ErrInval;
            fixed (byte* p = payload)
                return AtlasNetNative.AtlasNetSendCellRpc(_ctx, entityId, rpcId, p, payload.Length);
        }

        private void OnLoginNative(IntPtr userData, byte status,
                                   IntPtr baseappHostUtf8, ushort baseappPort,
                                   IntPtr errorUtf8)
        {
            string? err = errorUtf8 == IntPtr.Zero
                ? null : System.Runtime.InteropServices.Marshal.PtrToStringUTF8(errorUtf8);
            LoginFinished?.Invoke((AtlasLoginStatus)status, err);
        }

        private void OnAuthNative(IntPtr userData, byte success,
                                  uint entityId, ushort typeId, IntPtr errorUtf8)
        {
            string? err = errorUtf8 == IntPtr.Zero
                ? null : System.Runtime.InteropServices.Marshal.PtrToStringUTF8(errorUtf8);
            AuthFinished?.Invoke(success != 0, entityId, typeId, err);
        }

        void IAtlasNetEvents.OnDisconnect(int reason) => Disconnected?.Invoke(reason);

        void IAtlasNetEvents.OnPlayerBaseCreate(uint eid, ushort tid, ReadOnlySpan<byte> p)
            => PlayerBaseCreated?.Invoke(eid, tid, p.ToArray());

        void IAtlasNetEvents.OnPlayerCellCreate(uint sid,
                                                float px, float py, float pz,
                                                float dx, float dy, float dz,
                                                ReadOnlySpan<byte> p)
            => PlayerCellCreated?.Invoke(sid, new Vector3(px, py, pz),
                                         new Vector3(dx, dy, dz), p.ToArray());

        void IAtlasNetEvents.OnResetEntities() => EntitiesReset?.Invoke();

        void IAtlasNetEvents.OnEntityEnter(uint eid, ushort tid,
                                           float px, float py, float pz,
                                           float dx, float dy, float dz,
                                           ReadOnlySpan<byte> p)
            => EntityEntered?.Invoke(eid, tid, new Vector3(px, py, pz),
                                     new Vector3(dx, dy, dz), p.ToArray());

        void IAtlasNetEvents.OnEntityLeave(uint eid) => EntityLeft?.Invoke(eid);

        void IAtlasNetEvents.OnEntityPosition(uint eid,
                                              float px, float py, float pz,
                                              float dx, float dy, float dz,
                                              bool onGround)
            => EntityPositionUpdated?.Invoke(eid, new Vector3(px, py, pz),
                                             new Vector3(dx, dy, dz), onGround);

        void IAtlasNetEvents.OnEntityProperty(uint eid, byte scope, ReadOnlySpan<byte> d)
            => EntityPropertyUpdated?.Invoke(eid, scope, d.ToArray());

        void IAtlasNetEvents.OnForcedPosition(uint eid,
                                              float px, float py, float pz,
                                              float dx, float dy, float dz)
            => EntityForcedPosition?.Invoke(eid, new Vector3(px, py, pz),
                                            new Vector3(dx, dy, dz));

        void IAtlasNetEvents.OnRpc(uint eid, uint rid, ReadOnlySpan<byte> p)
            => Rpc?.Invoke(eid, rid, p.ToArray());
    }
}
