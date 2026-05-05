using System;
using Atlas.Client;
using Atlas.Client.Native;
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
            if (_ctx == IntPtr.Zero) return;
            AtlasNetNative.AtlasNetPoll(_ctx);
            ClientCallbacks.EntityManager.TickInterpolation(Time.deltaTime);
        }

        public bool TryGetInterpolatedTransform(uint entityId,
                                                out Vector3 pos, out Vector3 dir, out bool onGround)
        {
            var entity = ClientCallbacks.EntityManager.Get(entityId);
            if (entity != null &&
                entity.TryGetInterpolated(Time.timeAsDouble,
                    out Atlas.DataTypes.Vector3 atlasPos,
                    out Atlas.DataTypes.Vector3 atlasDir,
                    out onGround))
            {
                pos = atlasPos.ToUnity();
                dir = atlasDir.ToUnity();
                return true;
            }
            pos = default;
            dir = default;
            onGround = false;
            return false;
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
            // BaseApp's AuthenticateResult is the only signal that owner exists; net_client
            // never emits a typed "player base/cell create" message, so the manager spawns
            // the local entity on auth-success and lets AoI envelopes drive the rest.
            if (success != 0)
                ClientCallbacks.CreateEntity(entityId, typeId);
            AuthFinished?.Invoke(success != 0, entityId, typeId, err);
        }

        void IAtlasNetEvents.OnDisconnect(int reason) => Disconnected?.Invoke(reason);

        void IAtlasNetEvents.OnDeliver(ushort msgId, ReadOnlySpan<byte> payload)
            => ClientCallbacks.DeliverFromServer(msgId, payload);
    }
}
