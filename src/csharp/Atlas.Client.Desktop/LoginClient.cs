using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Atlas.Client;
using Atlas.Client.Native;
using Atlas.Core;
using Atlas.Coro;

namespace Atlas.Client.Desktop;

// Native callbacks fire inside Poll() on the same main thread, so no
// synchronisation is needed between the source and the callback.
public sealed unsafe class LoginClient : IDisposable
{
    private readonly IntPtr _ctx;
    private GCHandle _selfHandle;
    private LoginSource? _loginInflight;
    private AuthSource? _authInflight;
    private bool _disposed;

    public LoginClient()
    {
        _ctx = AtlasNetNative.Create();
        _selfHandle = GCHandle.Alloc(this, GCHandleType.Normal);
        var digest = ClientHost.EntityDefDigest;
        if (digest is not null)
        {
            fixed (byte* ptr = digest)
                AtlasNetNative.AtlasNetSetEntityDefDigest(_ctx, ptr, digest.Length);
        }
    }

    public int Poll()
    {
        ThrowIfDisposed();
        return AtlasNetNative.AtlasNetPoll(_ctx);
    }

    public AtlasNetState State
    {
        get
        {
            ThrowIfDisposed();
            return AtlasNetNative.AtlasNetGetState(_ctx);
        }
    }

    public AtlasTask<LoginResult> LoginAsync(string loginAppHost, ushort loginAppPort,
        string username, string passwordHash, AtlasCancellationToken ct = default)
    {
        ThrowIfDisposed();
        if (_loginInflight is not null)
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
            src.AttachCancelReg(ct.Register(static s => ((LoginSource)s!).OnCancellationRequested(), src));

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
        if (_authInflight is not null)
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
            src.AttachCancelReg(ct.Register(static s => ((AuthSource)s!).OnCancellationRequested(), src));

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
        // Native first: it cannot fire any more callbacks once destroyed.
        AtlasNetNative.AtlasNetDestroy(_ctx);
        _loginInflight?.OnNativeShutdown(); _loginInflight = null;
        _authInflight?.OnNativeShutdown(); _authInflight = null;
        if (_selfHandle.IsAllocated) _selfHandle.Free();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void ThrowIfDisposed()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(LoginClient));
    }

    private static Exception NativeRcException(string call, int rc)
    {
        var msg = ReadNativeError();
        return new InvalidOperationException(
            string.IsNullOrEmpty(msg) ? $"{call} returned rc={rc}"
                                      : $"{call} returned rc={rc}: {msg}");
    }

    private static string ReadNativeError()
    {
        var ptr = AtlasNetNative.AtlasNetGlobalLastError();
        return ptr == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(ptr) ?? "";
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static string ReadUtf8(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
    private static void OnLoginResultThunk(IntPtr userData, byte status, IntPtr hostUtf8,
        ushort port, IntPtr errMsgUtf8)
    {
        try
        {
            if (userData == IntPtr.Zero) return;
            var self = GCHandle.FromIntPtr(userData).Target as LoginClient;
            var src = self?._loginInflight;
            if (self is null || src is null) return;
            self._loginInflight = null;
            if (status == (byte)AtlasLoginStatus.Success)
            {
                src.SetResult(new LoginResult(ReadUtf8(hostUtf8), port));
            }
            else
            {
                src.SetException(new LoginFailedException(
                    (AtlasLoginStatus)status, ReadUtf8(errMsgUtf8)));
            }
        }
        catch (Exception ex) { ErrorBridge.SetError(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
    private static void OnAuthResultThunk(IntPtr userData, byte success, uint entityId,
        ushort typeId, IntPtr errMsgUtf8)
    {
        try
        {
            if (userData == IntPtr.Zero) return;
            var self = GCHandle.FromIntPtr(userData).Target as LoginClient;
            var src = self?._authInflight;
            if (self is null || src is null) return;
            self._authInflight = null;
            if (success != 0)
            {
                src.SetResult(new AuthResult(entityId, typeId));
            }
            else
            {
                src.SetException(new AuthFailedException(ReadUtf8(errMsgUtf8)));
            }
        }
        catch (Exception ex) { ErrorBridge.SetError(ex); }
    }

    private static readonly IntPtr s_loginCallbackPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, byte, IntPtr, ushort, IntPtr, void>)&OnLoginResultThunk;

    private static readonly IntPtr s_authCallbackPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, byte, uint, ushort, IntPtr, void>)&OnAuthResultThunk;

    private sealed class LoginSource : IAtlasTaskSource<LoginResult>
    {
        private readonly LoginClient _owner;
        private AtlasTaskCompletionSourceCore<LoginResult> _core;
        private CancelRegistration _cancelReg;

        public LoginSource(LoginClient owner) { _owner = owner; }
        public AtlasTask<LoginResult> Task => AtlasTask<LoginResult>.FromSource(this, _core.Version);

        public void AttachCancelReg(CancelRegistration r) => _cancelReg = r;
        public void SetResult(LoginResult v) { _core.TrySetResult(v); }
        public void SetException(Exception ex) { _core.TrySetException(ex); }

        // No AtlasNetCancelLogin in the C API; best-effort = disown +
        // disconnect; a late callback sees inflight == null and drops.
        public void OnCancellationRequested()
        {
            if (_owner._loginInflight != this) return;
            _owner._loginInflight = null;
            AtlasNetNative.AtlasNetDisconnect(_owner._ctx, AtlasDisconnectReason.User);
            _core.TrySetCanceled();
        }

        public void OnNativeShutdown() => _core.TrySetCanceled();

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

        public void AttachCancelReg(CancelRegistration r) => _cancelReg = r;
        public void SetResult(AuthResult v) { _core.TrySetResult(v); }
        public void SetException(Exception ex) { _core.TrySetException(ex); }

        public void OnCancellationRequested()
        {
            if (_owner._authInflight != this) return;
            _owner._authInflight = null;
            AtlasNetNative.AtlasNetDisconnect(_owner._ctx, AtlasDisconnectReason.User);
            _core.TrySetCanceled();
        }

        public void OnNativeShutdown() => _core.TrySetCanceled();

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

