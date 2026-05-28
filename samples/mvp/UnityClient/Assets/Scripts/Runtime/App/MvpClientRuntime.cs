using System;
using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;
using MvpAvatar = Atlas.Mvp.Client.Avatar;

namespace Atlas.Mvp.Unity
{
    public sealed class MvpClientRuntime : IDisposable
    {
        readonly AtlasNetworkManager _net;
        readonly AtlasUnityFramePump _frame;
        readonly CameraController _camera;
        readonly Material? _groundMaterial;
        readonly BotPilot? _botPilot;
        static readonly float[] s_dashCurveSamples = { 0.0f, 1.0f };
        const ushort kDashCurveId = 1;
        WorldLifecycle? _world;
        ViewRegistry? _views;

        public MvpClientRuntime(AtlasNetworkManager net, AtlasUnityFramePump frame,
            CameraController camera, Material? groundMaterial, BotPilot? botPilot)
        {
            _net = net;
            _frame = frame;
            _camera = camera;
            _groundMaterial = groundMaterial;
            _botPilot = botPilot;
            MovementCurves.Register(kDashCurveId, s_dashCurveSamples);
        }

        public event Action? LogoutRequested;
        public event Action? OwnerAttached;
        public uint OwnerEntityId => _views?.OwnerEntityId ?? 0;
        public Vector2 HudLookDelta => _world?.Hud?.ConsumeLookDelta() ?? Vector2.zero;

        public bool IsMoveInputActive() =>
            Mathf.Abs(Input.GetAxisRaw("Horizontal")) > 0.01f ||
            Mathf.Abs(Input.GetAxisRaw("Vertical")) > 0.01f ||
            (_world?.Hud?.JoystickInput.sqrMagnitude ?? 0f) > 0.01f ||
            (_botPilot != null && _botPilot.Joystick.sqrMagnitude > 0.01f);

        public void BuildWorld()
        {
            if (_world != null) return;
            _world = new WorldLifecycle(_net, _groundMaterial, _frame, () => _camera.Camera);
            _world.Build();
            _world.Hud!.LogoutRequested += OnHudLogoutRequested;
            _views = new ViewRegistry(_net, _world.WorldRoot!, _frame, _world.Labels!,
                OnOwnerAttached);
            _world.Hud!.BindViewRegistry(_views);
        }

        public void TeardownWorld()
        {
            _views?.Dispose();
            _views = null;
            Cursor.lockState = CursorLockMode.None;
            if (_world?.Hud != null)
                _world.Hud.LogoutRequested -= OnHudLogoutRequested;
            _world?.Dispose();
            _world = null;
            _camera.SetFollowTarget(null);
        }

        public void Dispose() => TeardownWorld();

        void OnHudLogoutRequested() => LogoutRequested?.Invoke();

        void OnOwnerAttached(AvatarView view, MvpAvatar avatar)
        {
            var joystickFn = _botPilot != null
                ? (Func<Vector2>)(() => _botPilot.Joystick)
                : () => _world?.Hud?.JoystickInput ?? Vector2.zero;
            var fireFn = _botPilot != null
                ? (Func<bool>)(() => _botPilot.ConsumeFire())
                : () => _world?.Hud?.ConsumeFireRequest() ?? false;
            var chatBlockedFn = _botPilot != null
                ? (Func<bool>)(() => false)
                : () => _world?.Hud?.IsChatFocused ?? false;
            view.AttachInput(new PlayerInputController(avatar, _net, view.Root.transform, _frame,
                joystickFn, fireFn, () => _camera.Yaw, chatBlockedFn));
            _world?.AoiBoxes?.Attach(view.Root.transform, 50f, 55f,
                new Color(0f, 1f, 0.4f, 0.7f),
                new Color(1f, 0.7f, 0.2f, 0.5f));
            _world?.Hud?.SetOwner(avatar, view.Root.transform);
            _camera.SetFollowTarget(view.Root.transform);
            OwnerAttached?.Invoke();
        }
    }
}
