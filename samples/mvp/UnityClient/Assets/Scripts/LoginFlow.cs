using System;
using Atlas.Client;
using Atlas.Client.Native;
using Atlas.Client.Unity;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public enum LoginFlowState
    {
        Idle,
        Connecting,
        Authenticating,
        EnteringWorld,
        InGame,
        Failed,
    }

    public sealed class LoginFlow
    {
        const float kBackoffBaseSec = 1f;
        const float kBackoffCapSec = 30f;

        readonly AtlasNetworkManager _net;
        LoginFlowState _state = LoginFlowState.Idle;
        string? _lastError;
        uint _accountEntityId;
        string? _cachedUser;
        string? _cachedHash;
        bool _hasCached;
        int _reconnectAttempts;
        float _reconnectClock;
        float _reconnectDueSec;
        bool _autoReconnect = true;

        public LoginFlowState State => _state;
        public string? LastError => _lastError;
        public uint AccountEntityId => _accountEntityId;
        public bool AutoReconnect { get => _autoReconnect; set => _autoReconnect = value; }
        public int ReconnectAttempts => _reconnectAttempts;
        public bool IsReconnectPending => _reconnectDueSec > 0f;
        public float ReconnectDelayRemaining
            => _reconnectDueSec > 0f ? Mathf.Max(0f, _reconnectDueSec - _reconnectClock) : 0f;

        public event Action<LoginFlowState>? StateChanged;

        public LoginFlow(AtlasNetworkManager net)
        {
            _net = net;
            _net.LoginFinished += OnLoginFinished;
            _net.AuthFinished += OnAuthFinished;
        }

        public void Dispose()
        {
            _net.LoginFinished -= OnLoginFinished;
            _net.AuthFinished -= OnAuthFinished;
        }

        public bool Begin(string username, string passwordHash)
        {
            if (_state != LoginFlowState.Idle && _state != LoginFlowState.Failed)
                return false;
            _cachedUser = username;
            _cachedHash = passwordHash;
            _hasCached = true;
            _reconnectDueSec = 0f;
            _lastError = null;
            Transition(LoginFlowState.Connecting);
            int rc = _net.Login(username, passwordHash);
            if (rc != AtlasNetReturnCode.Ok)
            {
                Fail($"Login submit failed (rc={rc})");
                return false;
            }
            return true;
        }

        // Bootstrap calls this on non-user-initiated disconnect; schedules
        // the next retry if auto-reconnect + cached credentials are both set.
        public void HandleDroppedConnection()
        {
            if (!_autoReconnect || !_hasCached)
            {
                Reset();
                return;
            }
            if (_state != LoginFlowState.Failed)
                Transition(LoginFlowState.Failed);
            if (_reconnectDueSec > 0f) return;
            _reconnectDueSec = _reconnectClock + ComputeBackoffSec(_reconnectAttempts);
        }

        public void TickReconnect(float dt)
        {
            _reconnectClock += dt;
            if (_reconnectDueSec <= 0f) return;
            if (_state != LoginFlowState.Failed) return;
            if (_reconnectClock < _reconnectDueSec) return;
            _reconnectDueSec = 0f;
            ++_reconnectAttempts;
            Begin(_cachedUser!, _cachedHash!);
        }

        static float ComputeBackoffSec(int attempts)
        {
            float scaled = kBackoffBaseSec * Mathf.Pow(2f, attempts);
            return Mathf.Min(scaled, kBackoffCapSec);
        }

        // Called by Bootstrap once the owner Avatar entity is bound; transitions
        // EnteringWorld → InGame so the UI can dismiss its loading state.
        public void NotifyEnteredWorld()
        {
            if (_state == LoginFlowState.EnteringWorld)
            {
                Transition(LoginFlowState.InGame);
                _reconnectAttempts = 0;
            }
        }

        public void Reset()
        {
            _accountEntityId = 0;
            _lastError = null;
            _reconnectDueSec = 0f;
            _reconnectAttempts = 0;
            Transition(LoginFlowState.Idle);
        }

        void OnLoginFinished(AtlasLoginStatus status, string? err)
        {
            if (_state != LoginFlowState.Connecting) return;
            if (status != AtlasLoginStatus.Success)
            {
                Fail($"{status}: {err ?? "(no detail)"}");
                return;
            }
            Transition(LoginFlowState.Authenticating);
            int rc = _net.Authenticate();
            if (rc != AtlasNetReturnCode.Ok)
                Fail($"Authenticate submit failed (rc={rc})");
        }

        void OnAuthFinished(bool success, uint entityId, ushort typeId, string? err)
        {
            if (_state != LoginFlowState.Authenticating) return;
            if (!success)
            {
                Fail(err ?? "auth rejected");
                return;
            }
            _accountEntityId = entityId;
            Transition(LoginFlowState.EnteringWorld);
            var account = _net.Session.EntityManager.Get(entityId)
                          as Atlas.Mvp.Client.Account;
            if (account == null)
            {
                Fail($"Account entity {entityId} missing after auth");
                return;
            }
            account.Base.SelectAvatar(1);
        }

        void Fail(string reason)
        {
            _lastError = reason;
            Debug.LogError($"[LoginFlow] Fail @ {_state}: {reason}");
            Transition(LoginFlowState.Failed);
            // Drop the native ctx back to Disconnected so the next Begin can
            // open a fresh connection; otherwise AtlasNetLogin returns EINVAL.
            _net.Logout();
            // Logout is a no-op when already disconnected — schedule directly
            // so a sync Login submit failure still drives the retry loop.
            if (_autoReconnect && _hasCached && _reconnectDueSec <= 0f)
                _reconnectDueSec = _reconnectClock + ComputeBackoffSec(_reconnectAttempts);
        }

        void Transition(LoginFlowState next)
        {
            if (_state == next) return;
            _state = next;
            try { StateChanged?.Invoke(next); }
            catch (Exception ex) { Debug.LogException(ex); }
        }
    }
}
