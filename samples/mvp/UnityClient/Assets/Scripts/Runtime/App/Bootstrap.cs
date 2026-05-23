using Atlas.Client.Unity;
using Atlas.Diagnostics;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public sealed class Bootstrap : MonoBehaviour
    {
        [SerializeField] string loginappHost = "127.0.0.1";
        [SerializeField] ushort loginappPort = 20018;
        [SerializeField] string username = "";
        [SerializeField] string passwordHash = "mvp_hash";
        [SerializeField] bool autoConnect = false;
        [SerializeField] Vector3 followOffset = new Vector3(0f, 2.5f, -4f);
        [SerializeField] float cameraLookHeight = 1.2f;
        [SerializeField] float positionSmoothTime = 0.12f;
        [SerializeField] float yawFollowSmoothTime = 0.25f;
        [SerializeField] float mouseSensitivity = 3f;
        [SerializeField] float hudLookSensitivity = 0.15f;
        [SerializeField] float pitchMin = -25f;
        [SerializeField] float pitchMax = 60f;
        [SerializeField] float initialPitch = 15f;
        [SerializeField] float zoomMin = 0.5f;
        [SerializeField] float zoomMax = 12f;
        [SerializeField] float zoomStep = 0.1f;
        [SerializeField] Material groundMaterial = null!;

        readonly AtlasUnityFramePump _frame = new();
        AtlasNetworkManager _net = null!;
        LoginScreen _loginScreen = null!;
        LoginFlow _flow = null!;
        CameraController _camera = null!;
        MvpClientRuntime? _runtime;
        bool _userInitiatedLogout;
        BotPilot? _botPilot;

        void Awake()
        {
            Application.runInBackground = true;
            Log.SetBackend(new UnityLogBackend());

            // Force script-DLL [ModuleInitializer] before Login or the digest goes zero.
            _ = Atlas.Rpc.EntityDefDigest.Bytes.Length;

            if (TryParseBotArgs(out string botUser, out float botDuration,
                    out BotPattern botPattern))
            {
                username = botUser;
                autoConnect = true;
                _botPilot = gameObject.AddComponent<BotPilot>();
                _botPilot.DurationSec = botDuration;
                _botPilot.Pattern = botPattern;
            }
            else if (string.IsNullOrEmpty(username))
                username = $"mvp_{System.Guid.NewGuid():N}".Substring(0, 12);

            _net = gameObject.AddComponent<AtlasNetworkManager>();
            _net.Configure(loginappHost, loginappPort);
            _net.Disconnected += OnNetDisconnected;

            _camera = new CameraController(CreateCamera(), BuildCameraConfig(), _frame,
                hudLookDelta: () => _runtime?.HudLookDelta ?? Vector2.zero,
                moveInputActive: () => _runtime?.IsMoveInputActive() ?? false);
            _runtime = new MvpClientRuntime(_net, _frame, _camera, groundMaterial, _botPilot);
            _runtime.LogoutRequested += OnHudLogoutRequested;

            _loginScreen = gameObject.AddComponent<LoginScreen>();
            _loginScreen.Configure(loginappHost, loginappPort, username, passwordHash);
            _loginScreen.LoginRequested += OnLoginRequested;
            _loginScreen.SetStatus("Enter credentials and press LOGIN", isError: false);

            _flow = new LoginFlow(_net);
            _flow.StateChanged += OnFlowStateChanged;
            _runtime.OwnerAttached += OnRuntimeOwnerAttached;
        }

        void Start()
        {
            if (autoConnect)
                BeginLogin(loginappHost, loginappPort, username, passwordHash);
        }

        void OnDestroy()
        {
            _flow?.Dispose();
            Cursor.lockState = CursorLockMode.None;
            if (_runtime != null)
            {
                _runtime.LogoutRequested -= OnHudLogoutRequested;
                _runtime.OwnerAttached -= OnRuntimeOwnerAttached;
                _runtime.Dispose();
            }
            _camera?.Dispose();
            _frame.Clear();
        }

        void Update()
        {
            _frame.RunTick(Time.deltaTime);
            _flow.TickReconnect(Time.deltaTime);
            RefreshReconnectStatus();
        }

        void RefreshReconnectStatus()
        {
            if (!_flow.IsReconnectPending) return;
            float remaining = _flow.ReconnectDelayRemaining;
            _loginScreen.SetStatus(
                $"Reconnecting in {remaining:0.0}s… (attempt #{_flow.ReconnectAttempts + 1})",
                isError: false);
        }

        // CameraController must register before LabelOverlay so
        // labels project against the post-follow camera pose.
        void LateUpdate() => _frame.RunLateTick();

        void OnLoginRequested(string host, ushort port, string user, string pwd) =>
            BeginLogin(host, port, user, pwd);

        void BeginLogin(string host, ushort port, string user, string pwd)
        {
            _net.Configure(host, port);
            _loginScreen.SetInteractable(false);
            _loginScreen.SetStatus("Connecting…", isError: false);
            if (!_flow.Begin(user, pwd))
                _loginScreen.SetInteractable(true);
        }

        void OnFlowStateChanged(LoginFlowState state)
        {
            switch (state)
            {
                case LoginFlowState.Connecting:
                    _loginScreen.SetStatus("Connecting to login server…", isError: false);
                    break;
                case LoginFlowState.Authenticating:
                    _loginScreen.SetStatus("Authenticating with base app…", isError: false);
                    break;
                case LoginFlowState.EnteringWorld:
                    _loginScreen.SetStatus("Loading world…", isError: false);
                    _runtime?.BuildWorld();
                    break;
                case LoginFlowState.InGame:
                    _loginScreen.Hide();
                    break;
                case LoginFlowState.Failed:
                    _loginScreen.Show();
                    _loginScreen.SetInteractable(true);
                    _loginScreen.SetStatus($"Login failed — {_flow.LastError}", isError: true);
                    _runtime?.TeardownWorld();
                    break;
            }
        }

        void OnNetDisconnected(int reason)
        {
            // Prefer LoginFlow.LastError over the generic disconnect reason —
            // Fail() already routed the useful message to the status line.
            string? carriedFailure = _flow.LastError;
            _runtime?.TeardownWorld();
            _loginScreen.Show();
            if (_userInitiatedLogout)
            {
                _userInitiatedLogout = false;
                _flow.Reset();
                _loginScreen.SetInteractable(true);
                _loginScreen.SetStatus("Logged out. Re-enter to play again.", isError: false);
                return;
            }
            _flow.HandleDroppedConnection();
            if (_flow.IsReconnectPending)
            {
                _loginScreen.SetInteractable(true);
                _loginScreen.SetStatus(
                    $"Disconnected (reason={reason}). Reconnecting…", isError: false);
                return;
            }
            _loginScreen.SetInteractable(true);
            if (carriedFailure == null)
                _loginScreen.SetStatus($"Disconnected (reason={reason}). Re-enter to retry.",
                    isError: true);
        }

        void OnHudLogoutRequested()
        {
            if (_userInitiatedLogout) return;
            _userInitiatedLogout = true;
            // Logout fires on_disconnect synchronously; defer so TeardownWorld
            // doesn't run inside the click handler that owns GameHud.
            Invoke(nameof(ExecuteUserLogout), 0f);
        }

        void ExecuteUserLogout() => _net.Logout();

        void OnRuntimeOwnerAttached() => _flow.NotifyEnteredWorld();

        Camera CreateCamera()
        {
            var camGo = new GameObject("MainCamera");
            camGo.transform.SetParent(transform, false);
            camGo.tag = "MainCamera";
            var cam = camGo.AddComponent<Camera>();
            cam.farClipPlane = 1500f;
            return cam;
        }

        CameraController.Config BuildCameraConfig() => new()
        {
            FollowOffset = followOffset,
            LookHeight = cameraLookHeight,
            PositionSmoothTime = positionSmoothTime,
            YawFollowSmoothTime = yawFollowSmoothTime,
            MouseSensitivity = mouseSensitivity,
            HudLookSensitivity = hudLookSensitivity,
            PitchMin = pitchMin, PitchMax = pitchMax, InitialPitch = initialPitch,
            ZoomMin = zoomMin, ZoomMax = zoomMax, ZoomStep = zoomStep,
        };

        static bool TryParseBotArgs(out string username, out float duration, out BotPattern pattern)
        {
            username = string.Empty;
            duration = 60f;
            pattern = BotPattern.Random;
            string[] args = System.Environment.GetCommandLineArgs();
            string? idx = null;
            for (int i = 0; i < args.Length; ++i)
            {
                if (args[i] == "-atlas-bot" && i + 1 < args.Length) idx = args[++i];
                else if (args[i] == "-atlas-bot-duration" && i + 1 < args.Length)
                    float.TryParse(args[++i], out duration);
                else if (args[i] == "-atlas-bot-pattern" && i + 1 < args.Length)
                {
                    string val = args[++i].ToLowerInvariant();
                    if (val == "pingpong") pattern = BotPattern.Pingpong;
                }
            }
            if (idx == null) return false;
            username = $"mvp_bot_{idx}";
            return true;
        }
    }
}
