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

        public void Configure(string host, ushort port)
        {
            loginappHost = host;
            loginappPort = port;
        }

        public event Action<AtlasLoginStatus, string?>? LoginFinished;
        public event Action<bool, uint, ushort, string?>? AuthFinished;
        public event Action<int>? Disconnected;

        public AtlasNetState State =>
            _ctx == IntPtr.Zero ? AtlasNetState.Disconnected : AtlasNetNative.AtlasNetGetState(_ctx);

        private IntPtr _ctx;
        private AtlasNetNative.LoginResultDelegate? _loginCallback;
        private AtlasNetNative.AuthResultDelegate? _authCallback;
        private uint _rpcOutCount;

        public uint RpcOutCount => _rpcOutCount;

        private void Awake()
        {
            _ctx = AtlasNetNative.Create();
            AtlasNetCallbackBridge.Register(_ctx, this);
            WireClientHostBridges();
        }

        // Unity has no DesktopBootstrap; install the ClientHost handler slots
        // here so generator-emitted Send*Rpc / SetEntityDefDigest reach native.
        private unsafe void WireClientHostBridges()
        {
            var cached = ClientHost.EntityDefDigest;
            if (cached != null && cached.Length > 0)
                fixed (byte* p = cached)
                    AtlasNetNative.AtlasNetSetEntityDefDigest(_ctx, p, cached.Length);

            ClientHost.SetEntityDefDigestHandler = data =>
            {
                if (_ctx == IntPtr.Zero || data.IsEmpty) return;
                fixed (byte* p = data)
                    AtlasNetNative.AtlasNetSetEntityDefDigest(_ctx, p, data.Length);
            };

            ClientHost.SendBaseRpcHandler = (entityId, rpcId, payload, _) =>
            {
                if (_ctx == IntPtr.Zero) return;
                fixed (byte* p = payload)
                    AtlasNetNative.AtlasNetSendBaseRpc(_ctx, entityId, rpcId, p, payload.Length);
                ++_rpcOutCount;
            };

            ClientHost.SendCellRpcHandler = (entityId, rpcId, payload, _) =>
            {
                if (_ctx == IntPtr.Zero) return;
                fixed (byte* p = payload)
                    AtlasNetNative.AtlasNetSendCellRpc(_ctx, entityId, rpcId, p, payload.Length);
                ++_rpcOutCount;
            };

            // net_client carries no entity-def registry; the no-ops just keep
            // ClientHost.Required from throwing on the script-DLL boot sweep.
            ClientHost.RegisterEntityTypeHandler = _ => { };
            ClientHost.RegisterStructHandler = _ => { };
            ClientHost.ReportEventSeqGapHandler = (_, _) => { };
        }

        private void Update()
        {
            if (_ctx == IntPtr.Zero) return;
            AtlasNetNative.AtlasNetPoll(_ctx);
            ClientCallbacks.EntityManager.TickInterpolation(Time.deltaTime);
        }

        public bool TryGetStats(out AtlasNetStats stats)
        {
            stats = default;
            if (_ctx == IntPtr.Zero) return false;
            return AtlasNetNative.AtlasNetGetStats(_ctx, out stats) == AtlasNetReturnCode.Ok;
        }

        public bool TryGetInterpolatedTransform(uint entityId,
                                                out Vector3 pos, out Vector3 dir, out bool onGround)
        {
            var entity = ClientCallbacks.EntityManager.Get(entityId);
            if (entity != null &&
                entity.TryGetInterpolated(
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
            {
                int rc = AtlasNetNative.AtlasNetSendBaseRpc(_ctx, entityId, rpcId, p, payload.Length);
                if (rc == AtlasNetReturnCode.Ok) ++_rpcOutCount;
                return rc;
            }
        }

        public unsafe int SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
        {
            if (_ctx == IntPtr.Zero) return AtlasNetReturnCode.ErrInval;
            fixed (byte* p = payload)
            {
                int rc = AtlasNetNative.AtlasNetSendCellRpc(_ctx, entityId, rpcId, p, payload.Length);
                if (rc == AtlasNetReturnCode.Ok) ++_rpcOutCount;
                return rc;
            }
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
            // AuthenticateResult is the only owner-create signal on the wire.
            if (success != 0)
                ClientCallbacks.CreateEntity(entityId, typeId);
            AuthFinished?.Invoke(success != 0, entityId, typeId, err);
        }

        void IAtlasNetEvents.OnDisconnect(int reason) => Disconnected?.Invoke(reason);

        void IAtlasNetEvents.OnDeliver(ushort msgId, ReadOnlySpan<byte> payload)
            => ClientCallbacks.DeliverFromServer(msgId, payload);
    }
}
