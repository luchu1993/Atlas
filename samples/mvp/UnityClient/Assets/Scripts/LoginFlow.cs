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
        readonly AtlasNetworkManager _net;
        LoginFlowState _state = LoginFlowState.Idle;
        string? _lastError;
        uint _accountEntityId;

        public LoginFlowState State => _state;
        public string? LastError => _lastError;
        public uint AccountEntityId => _accountEntityId;

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

        // Called by Bootstrap once the owner Avatar entity is bound; transitions
        // EnteringWorld → InGame so the UI can dismiss its loading state.
        public void NotifyEnteredWorld()
        {
            if (_state == LoginFlowState.EnteringWorld)
                Transition(LoginFlowState.InGame);
        }

        public void Reset()
        {
            _accountEntityId = 0;
            _lastError = null;
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
            var account = ClientCallbacks.EntityManager.Get(entityId)
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
