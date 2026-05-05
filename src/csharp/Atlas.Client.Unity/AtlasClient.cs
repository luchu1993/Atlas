using System;
using Atlas.Client;
using Atlas.Client.Native;
using Atlas.Coro;

namespace Atlas.Client.Unity
{
    // Mirror of Desktop AtlasClient; UnityLoop is ticked by PlayerLoop so
    // Update only polls native callbacks.
    public sealed class AtlasClient : IDisposable
    {
        private readonly LoginClient _login;
        private AtlasNetState _lastState = AtlasNetState.Disconnected;
        private bool _disposed;

        public AtlasClient() { _login = new LoginClient(); }

        public AtlasNetState State => _login.State;
        public LoginResult? LastLogin { get; private set; }
        public AuthResult? LastAuth { get; private set; }

        public event Action<AtlasNetState> StateChanged;

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
        }

        private void ThrowIfDisposed()
        {
            if (_disposed) throw new ObjectDisposedException(nameof(AtlasClient));
        }
    }
}
