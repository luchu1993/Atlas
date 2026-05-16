using Atlas.Client.Unity;
using Atlas.Diagnostics;
using UnityEngine;
using MvpAvatar = Atlas.Mvp.Client.Avatar;

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

        public static Bootstrap? Instance { get; private set; }
        public Camera? MainCamera => _camera?.Camera;
        public uint OwnerEntityId => _views?.OwnerEntityId ?? 0;

        AtlasNetworkManager _net = null!;
        LoginScreen _loginScreen = null!;
        LoginFlow _flow = null!;
        CameraController _camera = null!;
        WorldLifecycle? _world;
        ViewRegistry? _views;
        bool _userInitiatedLogout;

        void Awake()
        {
            Instance = this;
            Application.runInBackground = true;
            Log.SetBackend(new UnityLogBackend());

            // Force script-DLL [ModuleInitializer] before Login or the digest goes zero.
            _ = Atlas.Rpc.EntityDefDigest.Bytes.Length;

            if (string.IsNullOrEmpty(username))
                username = $"mvp_{System.Guid.NewGuid():N}".Substring(0, 12);

            _net = gameObject.AddComponent<AtlasNetworkManager>();
            _net.Configure(loginappHost, loginappPort);
            _net.Disconnected += OnNetDisconnected;

            _camera = new CameraController(CreateCamera(), BuildCameraConfig(),
                hudLookDelta: () => _world?.Hud?.ConsumeLookDelta() ?? Vector2.zero,
                moveInputActive: IsMoveInputActive);

            _loginScreen = gameObject.AddComponent<LoginScreen>();
            _loginScreen.Configure(loginappHost, loginappPort, username, passwordHash);
            _loginScreen.LoginRequested += OnLoginRequested;
            _loginScreen.SetStatus("Enter credentials and press LOGIN", isError: false);

            _flow = new LoginFlow(_net);
            _flow.StateChanged += OnFlowStateChanged;
        }

        void Start()
        {
            if (autoConnect)
                BeginLogin(loginappHost, loginappPort, username, passwordHash);
        }

        void OnDestroy()
        {
            if (Instance == this) Instance = null;
            _flow?.Dispose();
            Cursor.lockState = CursorLockMode.None;
            TeardownWorld();
            _camera?.Dispose();
        }

        void Update() => Ticker.RunTick(Time.deltaTime);

        // CameraController must register on Ticker before LabelOverlay so
        // labels project against the post-follow camera pose.
        void LateUpdate() => Ticker.RunLateTick();

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
                    BuildWorld();
                    break;
                case LoginFlowState.InGame:
                    _loginScreen.Hide();
                    break;
                case LoginFlowState.Failed:
                    _loginScreen.Show();
                    _loginScreen.SetInteractable(true);
                    _loginScreen.SetStatus($"Login failed — {_flow.LastError}", isError: true);
                    TeardownWorld();
                    break;
            }
        }

        void OnNetDisconnected(int reason)
        {
            // Prefer LoginFlow.LastError over the generic disconnect reason —
            // Fail() already routed the useful message to the status line.
            string? carriedFailure = _flow.LastError;
            TeardownWorld();
            _flow.Reset();
            _loginScreen.Show();
            _loginScreen.SetInteractable(true);
            if (_userInitiatedLogout)
            {
                _userInitiatedLogout = false;
                _loginScreen.SetStatus("Logged out. Re-enter to play again.", isError: false);
            }
            else if (carriedFailure == null)
            {
                _loginScreen.SetStatus($"Disconnected (reason={reason}). Re-enter to retry.",
                    isError: true);
            }
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

        void BuildWorld()
        {
            if (_world != null) return;
            _world = new WorldLifecycle(_net, groundMaterial);
            _world.Build();
            _world.Hud!.LogoutRequested += OnHudLogoutRequested;
            _views = new ViewRegistry(_net, _world.WorldRoot!, OnOwnerAttached);
        }

        void TeardownWorld()
        {
            _views?.Dispose();
            _views = null;
            Cursor.lockState = CursorLockMode.None;
            _world?.Dispose();
            _world = null;
            _camera?.SetFollowTarget(null);
        }

        void OnOwnerAttached(AvatarView view, MvpAvatar avatar)
        {
            view.AttachInput(new PlayerInputController(avatar, _net, view.Root.transform,
                () => _world?.Hud?.JoystickInput ?? Vector2.zero,
                () => _world?.Hud?.ConsumeFireRequest() ?? false,
                () => _camera.Yaw));
            AoiBoxes.Attach(view.Root.transform, 50f, 55f,
                new Color(0f, 1f, 0.4f, 0.7f),
                new Color(1f, 0.7f, 0.2f, 0.5f));
            _world?.Hud?.SetOwner(avatar);
            _camera.SetFollowTarget(view.Root.transform);
            _flow.NotifyEnteredWorld();
        }

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

        bool IsMoveInputActive() =>
            Mathf.Abs(Input.GetAxisRaw("Horizontal")) > 0.01f ||
            Mathf.Abs(Input.GetAxisRaw("Vertical")) > 0.01f ||
            (_world?.Hud?.JoystickInput.sqrMagnitude ?? 0f) > 0.01f;
    }
}
