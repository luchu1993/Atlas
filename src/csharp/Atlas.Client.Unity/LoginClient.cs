using System;
using System.Runtime.InteropServices;
using AOT;
using Atlas.Client;
using Atlas.Client.Native;
using Atlas.Coro;

namespace Atlas.Client.Unity
{
    // Mirror of Desktop LoginClient; differs only in the native-callback
    // bridge (MonoPInvokeCallback instead of UnmanagedCallersOnly).
    public sealed class LoginClient : IDisposable
    {
        private readonly IntPtr _ctx;
        private GCHandle _selfHandle;
        private LoginSource _loginInflight;
        private AuthSource _authInflight;
        private bool _disposed;

        public LoginClient()
        {
            _ctx = AtlasNetNative.Create();
            _selfHandle = GCHandle.Alloc(this, GCHandleType.Normal);
        }

        public int Poll()
        {
            ThrowIfDisposed();
            return AtlasNetNative.AtlasNetPoll(_ctx);
        }

        public AtlasNetState State
        {
            get { ThrowIfDisposed(); return AtlasNetNative.AtlasNetGetState(_ctx); }
        }

        public AtlasTask<LoginResult> LoginAsync(string loginAppHost, ushort loginAppPort,
            string username, string passwordHash, AtlasCancellationToken ct = default)
        {
            ThrowIfDisposed();
            if (_loginInflight != null)
                return AtlasTask<LoginResult>.FromException(
                    new InvalidOperationException("LoginClient: another login is already in flight"));

            var src = new LoginSource(this);
            _loginInflight = src;

            if (ct.IsCancellationRequested)
            {
                _loginInflight = null;
                return AtlasTask<LoginResult>.FromCanceled();
            }
            if (ct.CanBeCanceled)
                src.AttachCancelReg(ct.Register(static s => ((LoginSource)s).OnCancellationRequested(), src));

            var rc = AtlasNetNative.AtlasNetLogin(_ctx, loginAppHost, loginAppPort, username,
                passwordHash, s_loginCallbackPtr, GCHandle.ToIntPtr(_selfHandle));
            if (rc != AtlasNetReturnCode.Ok)
            {
                _loginInflight = null;
                return AtlasTask<LoginResult>.FromException(
                    NativeRcException("AtlasNetLogin", rc));
            }
            return src.Task;
        }

        public AtlasTask<AuthResult> AuthenticateAsync(AtlasCancellationToken ct = default)
        {
            ThrowIfDisposed();
            if (_authInflight != null)
                return AtlasTask<AuthResult>.FromException(
                    new InvalidOperationException("LoginClient: another authenticate is already in flight"));

            var src = new AuthSource(this);
            _authInflight = src;

            if (ct.IsCancellationRequested)
            {
                _authInflight = null;
                return AtlasTask<AuthResult>.FromCanceled();
            }
            if (ct.CanBeCanceled)
                src.AttachCancelReg(ct.Register(static s => ((AuthSource)s).OnCancellationRequested(), src));

            var rc = AtlasNetNative.AtlasNetAuthenticate(_ctx, s_authCallbackPtr,
                GCHandle.ToIntPtr(_selfHandle));
            if (rc != AtlasNetReturnCode.Ok)
            {
                _authInflight = null;
                return AtlasTask<AuthResult>.FromException(
                    NativeRcException("AtlasNetAuthenticate", rc));
            }
            return src.Task;
        }

        public void Disconnect(AtlasDisconnectReason reason = AtlasDisconnectReason.User)
        {
            if (_disposed) return;
            AtlasNetNative.AtlasNetDisconnect(_ctx, reason);
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            AtlasNetNative.AtlasNetDestroy(_ctx);
            _loginInflight?.OnNativeShutdown(); _loginInflight = null;
            _authInflight?.OnNativeShutdown(); _authInflight = null;
            if (_selfHandle.IsAllocated) _selfHandle.Free();
        }

        private void ThrowIfDisposed()
        {
            if (_disposed) throw new ObjectDisposedException(nameof(LoginClient));
        }

        private static Exception NativeRcException(string call, int rc)
        {
            var ptr = AtlasNetNative.AtlasNetGlobalLastError();
            var msg = ptr == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(ptr) ?? "";
            return new InvalidOperationException(
                string.IsNullOrEmpty(msg) ? string.Format("{0} returned rc={1}", call, rc)
                                          : string.Format("{0} returned rc={1}: {2}", call, rc, msg));
        }

        private static string ReadUtf8(IntPtr p)
            => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";

        // Cached delegate instances keep the trampoline alive past IL2CPP AOT.
        private static readonly AtlasNetNative.LoginResultDelegate s_loginCallback = OnLoginResult;
        private static readonly AtlasNetNative.AuthResultDelegate s_authCallback = OnAuthResult;
        private static readonly IntPtr s_loginCallbackPtr =
            Marshal.GetFunctionPointerForDelegate(s_loginCallback);
        private static readonly IntPtr s_authCallbackPtr =
            Marshal.GetFunctionPointerForDelegate(s_authCallback);

        [MonoPInvokeCallback(typeof(AtlasNetNative.LoginResultDelegate))]
        private static void OnLoginResult(IntPtr userData, byte status, IntPtr hostUtf8,
            ushort port, IntPtr errMsgUtf8)
        {
            try
            {
                if (userData == IntPtr.Zero) return;
                var self = GCHandle.FromIntPtr(userData).Target as LoginClient;
                var src = self?._loginInflight;
                if (self == null || src == null) return;
                self._loginInflight = null;
                if (status == (byte)AtlasLoginStatus.Success)
                    src.SetResult(new LoginResult(ReadUtf8(hostUtf8), port));
                else
                    src.SetException(new LoginFailedException(
                        (AtlasLoginStatus)status, ReadUtf8(errMsgUtf8)));
            }
            catch (Exception ex)
            {
                UnityEngine.Debug.LogException(ex);
            }
        }

        [MonoPInvokeCallback(typeof(AtlasNetNative.AuthResultDelegate))]
        private static void OnAuthResult(IntPtr userData, byte success, uint entityId,
            ushort typeId, IntPtr errMsgUtf8)
        {
            try
            {
                if (userData == IntPtr.Zero) return;
                var self = GCHandle.FromIntPtr(userData).Target as LoginClient;
                var src = self?._authInflight;
                if (self == null || src == null) return;
                self._authInflight = null;
                if (success != 0)
                    src.SetResult(new AuthResult(entityId, typeId));
                else
                    src.SetException(new AuthFailedException(ReadUtf8(errMsgUtf8)));
            }
            catch (Exception ex)
            {
                UnityEngine.Debug.LogException(ex);
            }
        }

        private sealed class LoginSource : IAtlasTaskSource<LoginResult>
        {
            private readonly LoginClient _owner;
            private AtlasTaskCompletionSourceCore<LoginResult> _core;
            private CancelRegistration _cancelReg;

            public LoginSource(LoginClient owner) { _owner = owner; }
            public AtlasTask<LoginResult> Task => AtlasTask<LoginResult>.FromSource(this, _core.Version);

            public void AttachCancelReg(CancelRegistration r) { _cancelReg = r; }
            public void SetResult(LoginResult v) { _core.TrySetResult(v); }
            public void SetException(Exception ex) { _core.TrySetException(ex); }

            public void OnCancellationRequested()
            {
                if (_owner._loginInflight != this) return;
                _owner._loginInflight = null;
                AtlasNetNative.AtlasNetDisconnect(_owner._ctx, AtlasDisconnectReason.User);
                _core.TrySetCanceled();
            }

            public void OnNativeShutdown() { _core.TrySetCanceled(); }

            public short Version => _core.Version;
            public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
            public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
            public LoginResult GetResult(short t)
            {
                try { return _core.GetResult(t); }
                finally { _cancelReg.Dispose(); _cancelReg = default; }
            }
        }

        private sealed class AuthSource : IAtlasTaskSource<AuthResult>
        {
            private readonly LoginClient _owner;
            private AtlasTaskCompletionSourceCore<AuthResult> _core;
            private CancelRegistration _cancelReg;

            public AuthSource(LoginClient owner) { _owner = owner; }
            public AtlasTask<AuthResult> Task => AtlasTask<AuthResult>.FromSource(this, _core.Version);

            public void AttachCancelReg(CancelRegistration r) { _cancelReg = r; }
            public void SetResult(AuthResult v) { _core.TrySetResult(v); }
            public void SetException(Exception ex) { _core.TrySetException(ex); }

            public void OnCancellationRequested()
            {
                if (_owner._authInflight != this) return;
                _owner._authInflight = null;
                AtlasNetNative.AtlasNetDisconnect(_owner._ctx, AtlasDisconnectReason.User);
                _core.TrySetCanceled();
            }

            public void OnNativeShutdown() { _core.TrySetCanceled(); }

            public short Version => _core.Version;
            public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
            public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
            public AuthResult GetResult(short t)
            {
                try { return _core.GetResult(t); }
                finally { _cancelReg.Dispose(); _cancelReg = default; }
            }
        }
    }
}
