using System.Collections.Generic;
using Atlas.Client;
using Atlas.Client.Native;
using Atlas.Client.Unity;
using Atlas.Diagnostics;
using UnityEngine;
using MvpAvatar = Atlas.Mvp.Client.Avatar;
using MvpNpc = Atlas.Mvp.Client.Npc;

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
        [SerializeField] float pitchMin = -25f;
        [SerializeField] float pitchMax = 60f;
        [SerializeField] float initialPitch = 15f;
        [SerializeField] float zoomMin = 0.5f;
        [SerializeField] float zoomMax = 12f;
        [SerializeField] float zoomStep = 0.1f;
        [SerializeField] Material groundMaterial = null!;

        public static Bootstrap? Instance { get; private set; }
        public float CameraYaw => _yaw;
        public Camera? MainCamera => _camera;
        public uint OwnerEntityId => _ownerEntityId;

        AtlasNetworkManager _net = null!;
        LoginScreen _loginScreen = null!;
        LoginFlow _flow = null!;
        readonly Dictionary<uint, EntityView> _views = new();
        readonly List<uint> _stale = new();
        GameObject? _worldRoot;
        Camera? _camera;
        GameHud? _hud;
        ProjectileVisualController? _projectiles;
        uint _ownerEntityId;
        float _zoom = 1.0f;
        Vector3 _camVelocity;
        float _yaw;
        float _pitch;
        float _yawVelocity;
        bool _cameraInitialized;
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

            CreateCamera();
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
        }

        void Update()
        {
            if (_worldRoot == null) return;
            float scroll = Input.mouseScrollDelta.y;
            if (Mathf.Abs(scroll) > 0.01f)
                _zoom = Mathf.Clamp(_zoom * Mathf.Pow(1f - zoomStep, scroll), zoomMin, zoomMax);
            ReconcileViews();
            Ticker.RunTick(Time.deltaTime);
        }

        // FollowOwner updates the camera; LateTick must run after it so
        // label projection samples the post-follow camera pose.
        void LateUpdate()
        {
            if (_worldRoot == null || _camera == null) return;
            FollowOwner();
            Ticker.RunLateTick();
        }

        void OnLoginRequested(string host, ushort port, string user, string pwd)
        {
            BeginLogin(host, port, user, pwd);
        }

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
            // LoginFlow.Fail triggers a logout to reset the native ctx; its
            // error string is already on the status line and is more useful
            // than the generic reason code, so keep it.
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
            if (_worldRoot != null) return;
            _worldRoot = new GameObject("World");

            var ground = GameObject.CreatePrimitive(PrimitiveType.Plane);
            ground.name = "Ground";
            ground.transform.SetParent(_worldRoot.transform, false);
            // Primitive plane is 10m; scale 20× → 200m world.
            ground.transform.localScale = new Vector3(20f, 1f, 20f);
            var renderer = ground.GetComponent<Renderer>();
            if (groundMaterial != null)
            {
                renderer.material = groundMaterial;
                renderer.material.mainTextureScale = new Vector2(100f, 100f);
            }
            else
            {
                renderer.material.color = new Color(0.55f, 0.55f, 0.55f);
            }

            var lightGo = new GameObject("DirectionalLight");
            lightGo.transform.SetParent(_worldRoot.transform, false);
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 1.0f;
            lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);

            // Camera is persistent (created in Awake); switch its clear color
            // to the in-world sky so the world reads correctly.
            if (_camera != null)
            {
                _camera.backgroundColor = new Color(0.5f, 0.6f, 0.7f);
                _camera.transform.position = new Vector3(0f, 5f, -8f);
                _camera.transform.LookAt(Vector3.zero);
            }

            var hudGo = new GameObject("HudRoot");
            hudGo.transform.SetParent(_worldRoot.transform, false);
            _hud = hudGo.AddComponent<GameHud>();
            _hud.Bind(_net);
            _hud.LogoutRequested += OnHudLogoutRequested;
            LabelOverlay.Init();
            _projectiles = new ProjectileVisualController();

            _cameraInitialized = false;
            _zoom = 1.0f;
            _camVelocity = Vector3.zero;
            _yawVelocity = 0f;
        }

        void TeardownWorld()
        {
            _ownerEntityId = 0;
            Cursor.lockState = CursorLockMode.None;
            foreach (var v in _views.Values) v?.Dispose();
            _views.Clear();
            _hud = null;
            _projectiles?.Dispose();
            _projectiles = null;
            AoiBoxes.Clear();
            LabelOverlay.Shutdown();
            DrainStaleEntities();
            if (_worldRoot != null) { Destroy(_worldRoot); _worldRoot = null; }
            Ticker.Clear();
            ResetCameraToBootPose();
        }

        // net_client fires no entity-destroyed callbacks on disconnect, so
        // ClientCallbacks.EntityManager keeps the previous session's entities.
        // A fast re-login would otherwise have ReconcileViews spawn a ghost
        // copy of the prior owner Avatar.
        static void DrainStaleEntities()
        {
            var em = ClientCallbacks.EntityManager;
            var stale = new List<uint>();
            foreach (var e in em.Entities) stale.Add(e.EntityId);
            foreach (var id in stale) em.Destroy(id);
        }

        void CreateCamera()
        {
            var camGo = new GameObject("MainCamera");
            camGo.transform.SetParent(transform, false);
            camGo.tag = "MainCamera";
            _camera = camGo.AddComponent<Camera>();
            _camera.farClipPlane = 1500f;
            ResetCameraToBootPose();
        }

        void ResetCameraToBootPose()
        {
            if (_camera == null) return;
            _camera.backgroundColor = new Color(0.05f, 0.05f, 0.07f);
            _camera.clearFlags = CameraClearFlags.SolidColor;
            _camera.transform.position = new Vector3(0f, 5f, -8f);
            _camera.transform.LookAt(Vector3.zero);
            _cameraInitialized = false;
        }

        void ReconcileViews()
        {
            foreach (var entity in ClientCallbacks.EntityManager.Entities)
            {
                if (_views.ContainsKey(entity.EntityId)) continue;
                if (entity is MvpAvatar || entity is MvpNpc)
                    _views[entity.EntityId] = SpawnView(entity);
            }
            _stale.Clear();
            foreach (var kv in _views)
            {
                if (kv.Value == null || kv.Value.Entity == null || kv.Value.Entity.IsDestroyed)
                    _stale.Add(kv.Key);
            }
            foreach (var id in _stale)
            {
                _views[id]?.Dispose();
                _views.Remove(id);
            }
        }

        EntityView SpawnView(ClientEntity entity)
        {
            // ReconcileViews already filters to Avatar / Npc; the default arm
            // exists only because switch expressions must be exhaustive.
            EntityView view = entity switch
            {
                MvpAvatar a => new AvatarView(a, _net, _worldRoot!.transform),
                MvpNpc n => new NpcView(n, _net, _worldRoot!.transform),
                _ => throw new System.InvalidOperationException(
                         $"SpawnView: unsupported entity type {entity.TypeName}"),
            };

            if (entity.IsOwner && entity is MvpAvatar avatar)
            {
                // Owner may switch via EntityTransferred (Account → Avatar handoff).
                _ownerEntityId = entity.EntityId;
                ((AvatarView)view).AttachInput(
                    new PlayerInputController(avatar, _net, view.Root.transform));
                AoiBoxes.Attach(view.Root.transform, 50f, 55f,
                    new Color(0f, 1f, 0.4f, 0.7f),
                    new Color(1f, 0.7f, 0.2f, 0.5f));
                _hud?.SetOwner(avatar);
                _flow.NotifyEnteredWorld();
            }
            return view;
        }

        void FollowOwner()
        {
            if (_ownerEntityId == 0 || _camera == null) return;
            if (!_views.TryGetValue(_ownerEntityId, out var owner) || owner == null) return;
            var ownerTransform = owner.Root.transform;
            var targetPos = ownerTransform.position;
            var ownerYaw = ownerTransform.rotation.eulerAngles.y;

            if (!_cameraInitialized)
            {
                _yaw = ownerYaw;
                _pitch = initialPitch;
                _cameraInitialized = true;
            }

            if (Input.GetMouseButton(1))
            {
                Cursor.lockState = CursorLockMode.Locked;
                _yaw += Input.GetAxis("Mouse X") * mouseSensitivity;
                _pitch -= Input.GetAxis("Mouse Y") * mouseSensitivity;
                _pitch = Mathf.Clamp(_pitch, pitchMin, pitchMax);
            }
            else
            {
                Cursor.lockState = CursorLockMode.None;
                if (IsMoveInputActive())
                    _yaw = Mathf.SmoothDampAngle(_yaw, ownerYaw, ref _yawVelocity, yawFollowSmoothTime);
            }

            var orbitRot = Quaternion.Euler(_pitch, _yaw, 0f);
            var desiredPos = targetPos
                + Vector3.up * (followOffset.y * _zoom)
                + orbitRot * Vector3.back * (Mathf.Abs(followOffset.z) * _zoom);
            _camera.transform.position = Vector3.SmoothDamp(
                _camera.transform.position, desiredPos, ref _camVelocity, positionSmoothTime);
            _camera.transform.LookAt(targetPos + Vector3.up * cameraLookHeight);
        }

        static bool IsMoveInputActive() =>
            Mathf.Abs(Input.GetAxisRaw("Horizontal")) > 0.01f ||
            Mathf.Abs(Input.GetAxisRaw("Vertical")) > 0.01f;
    }
}
