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
        // Empty → auto-generate a unique username at Awake so two Editors / Standalone
        // builds can co-login. Set explicitly in Inspector to pin a name for repro.
        [SerializeField] string username = "";
        [SerializeField] string passwordHash = "mvp_hash";
        [SerializeField] bool autoConnect = true;
        // Orbit camera: .y = height above avatar; |.z| = orbit radius.
        [SerializeField] Vector3 followOffset = new Vector3(0f, 2.5f, -4f);
        [SerializeField] float cameraLookHeight = 1.2f;
        [SerializeField] float positionSmoothTime = 0.12f;
        // Yaw catches up to avatar facing while the player is moving.
        [SerializeField] float yawFollowSmoothTime = 0.25f;
        [SerializeField] float mouseSensitivity = 3f;
        [SerializeField] float pitchMin = -25f;
        [SerializeField] float pitchMax = 60f;
        [SerializeField] float initialPitch = 15f;
        [SerializeField] float zoomMin = 0.5f;
        [SerializeField] float zoomMax = 6f;
        [SerializeField] float zoomStep = 0.1f;
        // Inspector slot for the grid material; null falls back to plain gray.
        [SerializeField] Material groundMaterial = null!;

        public static Bootstrap? Instance { get; private set; }
        public float CameraYaw => _yaw;
        public Camera MainCamera => _camera;

        AtlasNetworkManager _net = null!;
        readonly Dictionary<uint, AvatarView> _views = new();
        Camera _camera = null!;
        uint _ownerEntityId;
        float _zoom = 1.0f;
        Vector3 _camVelocity;
        float _yaw;
        float _pitch;
        float _yawVelocity;
        bool _cameraInitialized;

        void Awake()
        {
            Instance = this;
            // Without this, Editor loses focus → Update halts → RUDP inbound stalls.
            Application.runInBackground = true;
            Log.SetBackend(new UnityLogBackend());

            // Force script-DLL [ModuleInitializer] before Login or the digest goes zero.
            _ = Atlas.Rpc.EntityDefDigest.Bytes.Length;

            if (string.IsNullOrEmpty(username))
                username = $"mvp_{System.Guid.NewGuid():N}".Substring(0, 12);
            Debug.Log($"[Mvp.Bootstrap] login username={username}");

            BuildScene();
            _net = gameObject.AddComponent<AtlasNetworkManager>();
            _net.Configure(loginappHost, loginappPort);
            _net.LoginFinished += OnLoginFinished;
            _net.AuthFinished += OnAuthFinished;
            _net.Disconnected += OnDisconnected;
            gameObject.AddComponent<HudOverlay>().Bind(_net);
            gameObject.AddComponent<ProjectileVisualController>();
            gameObject.AddComponent<LabelOverlay>();
        }

        void Start()
        {
            if (autoConnect) _net.Login(username, passwordHash);
        }

        void Update()
        {
            float scroll = Input.mouseScrollDelta.y;
            if (Mathf.Abs(scroll) > 0.01f)
                _zoom = Mathf.Clamp(_zoom * Mathf.Pow(1f - zoomStep, scroll), zoomMin, zoomMax);
            ReconcileViews();
        }

        // LateUpdate: PlayerInputController writes the owner transform in
        // Update; reading it earlier costs a 1-frame lag.
        void LateUpdate()
        {
            FollowOwner();
        }

        void OnLoginFinished(AtlasLoginStatus status, string? err)
        {
            if (status != AtlasLoginStatus.Success)
            {
                Debug.LogError($"[Mvp.Bootstrap] Login failed: {status} {err}");
                return;
            }
            _net.Authenticate();
        }

        void OnAuthFinished(bool success, uint entityId, ushort typeId, string? err)
        {
            if (!success)
            {
                Debug.LogError($"[Mvp.Bootstrap] Auth failed: {err}");
                return;
            }
            _ownerEntityId = entityId;
            Debug.Log($"[Mvp.Bootstrap] Authenticated owner={entityId} typeId={typeId}");
            SendSelectAvatar(entityId);
        }

        void OnDisconnected(int reason)
        {
            Debug.LogWarning($"[Mvp.Bootstrap] Disconnected reason={reason}");
        }

        void SendSelectAvatar(uint accountEntityId)
        {
            var account = ClientCallbacks.EntityManager.Get(accountEntityId)
                          as Atlas.Mvp.Client.Account;
            if (account == null)
            {
                Debug.LogError($"[Mvp.Bootstrap] Account entity {accountEntityId} not found");
                return;
            }
            account.Base.SelectAvatar(1);
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
                if (_views[id] != null) Destroy(_views[id].gameObject);
                _views.Remove(id);
            }
        }

        readonly List<uint> _stale = new();

        AvatarView SpawnView(ClientEntity entity)
        {
            var go = new GameObject($"View_{entity.TypeName}_{entity.EntityId}");
            var view = go.AddComponent<AvatarView>();
            view.Bind(entity, _net);
            if (entity.IsOwner && entity is MvpAvatar avatar)
            {
                // Owner may switch via EntityTransferred (Account → Avatar handoff).
                _ownerEntityId = entity.EntityId;
                go.AddComponent<PlayerInputController>().Bind(avatar, _net);
                go.AddComponent<AttackInputController>().Bind(avatar);
                // Inner = enter boundary, outer = enter + hysteresis (leave boundary).
                go.AddComponent<AoIDebugRing>().Configure(
                    50f, 55f,
                    new Color(0f, 1f, 0.4f, 0.7f),
                    new Color(1f, 0.7f, 0.2f, 0.5f));
            }
            return view;
        }

        void FollowOwner()
        {
            if (_ownerEntityId == 0) return;
            if (!_views.TryGetValue(_ownerEntityId, out var owner) || owner == null) return;
            var targetPos = owner.transform.position;
            var ownerYaw = owner.transform.rotation.eulerAngles.y;

            if (!_cameraInitialized)
            {
                _yaw = ownerYaw;
                _pitch = initialPitch;
                _cameraInitialized = true;
            }

            // Right-button drag drives free orbit; locking the cursor avoids
            // mouse-edge clamping mid-spin and matches PUBG/MMO convention.
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

        void BuildScene()
        {
            var ground = GameObject.CreatePrimitive(PrimitiveType.Plane);
            ground.name = "Ground";
            // Primitive plane is 10m; scale 20× → 200m world.
            ground.transform.localScale = new Vector3(20f, 1f, 20f);
            var renderer = ground.GetComponent<Renderer>();
            if (groundMaterial != null)
            {
                renderer.material = groundMaterial;
                // 200m / 2-cells-per-tile texture → 100 tiles = 1m per cell.
                renderer.material.mainTextureScale = new Vector2(100f, 100f);
            }
            else
            {
                renderer.material.color = new Color(0.55f, 0.55f, 0.55f);
            }

            var lightGo = new GameObject("DirectionalLight");
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 1.0f;
            lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);

            var camGo = new GameObject("MainCamera");
            camGo.tag = "MainCamera";
            _camera = camGo.AddComponent<Camera>();
            _camera.farClipPlane = 1500f;
            _camera.backgroundColor = new Color(0.5f, 0.6f, 0.7f);
            // Initial pose before owner spawns; FollowOwner takes over once
            // the owner avatar enters the scene.
            camGo.transform.position = new Vector3(0f, 5f, -8f);
            camGo.transform.LookAt(Vector3.zero);
        }

        void OnDestroy()
        {
            if (Instance == this) Instance = null;
            Cursor.lockState = CursorLockMode.None;
            foreach (var v in _views.Values)
                if (v != null) Destroy(v.gameObject);
            _views.Clear();
        }
    }
}
