using System;
using Atlas.Client.Native;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Atlas.Coro.Rpc;

namespace Atlas.Client.Desktop;

public sealed class AtlasClient : IDisposable
{
    private readonly LoginClient _login;
    private readonly bool _ownsCoroLoop;
    private readonly ManagedAtlasLoop? _loop;
    private readonly ManagedRpcRegistry? _rpcRegistry;
    private AtlasNetState _lastState = AtlasNetState.Disconnected;
    private bool _disposed;

    public AtlasClient() : this(installCoroLoop: true) { }

    // Pass installCoroLoop=false when the host already installed AtlasLoop +
    // AtlasRpcRegistryHost (e.g. embedded CoreCLR via DesktopLifecycle).
    public AtlasClient(bool installCoroLoop)
    {
        _login = new LoginClient();
        _ownsCoroLoop = installCoroLoop;
        if (installCoroLoop)
        {
            _loop = new ManagedAtlasLoop();
            AtlasLoop.Install(_loop);
            _rpcRegistry = new ManagedRpcRegistry(_loop);
            AtlasRpcRegistryHost.Install(_rpcRegistry);
        }
    }

    public AtlasNetState State => _login.State;
    public LoginResult? LastLogin { get; private set; }
    public AuthResult? LastAuth { get; private set; }

    public event Action<AtlasNetState>? StateChanged;

    public async AtlasTask ConnectAsync(string loginAppHost, ushort loginAppPort,
        string username, string passwordHash, AtlasCancellationToken ct = default)
    {
        ThrowIfDisposed();
        LastLogin = await _login.LoginAsync(loginAppHost, loginAppPort, username, passwordHash, ct);
        LastAuth = await _login.AuthenticateAsync(ct);
    }

    public void Update()
    {
        if (_disposed) return;
        _login.Poll();
        _loop?.Drain();
        var s = _login.State;
        if (s != _lastState)
        {
            _lastState = s;
            StateChanged?.Invoke(s);
        }
    }

    public void Disconnect(AtlasDisconnectReason reason = AtlasDisconnectReason.User)
    {
        if (_disposed) return;
        _login.Disconnect(reason);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _login.Dispose();
        if (_ownsCoroLoop)
        {
            AtlasRpcRegistryHost.Reset();
            _rpcRegistry?.Dispose();
            _loop?.Dispose();
            AtlasLoop.Reset();
        }
    }

    private void ThrowIfDisposed()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(AtlasClient));
    }
}
