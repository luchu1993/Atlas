using System;
using Atlas.Client.Unity;
using UnityEngine;
using AtlasVec = Atlas.DataTypes.Vector3;
using MvpAvatar = Atlas.Mvp.Client.Avatar;

namespace Atlas.Mvp.Unity
{
    public sealed class PlayerInputController : ITickable, IDisposable
    {
        const float kMoveSpeed = 5f;
        const float kReportHz = 20f;
        const float kMoveTurnRate = 720f;
        const float kIdleTurnRate = 90f;
        const float kIdleTurnThresholdDeg = 45f;

        readonly MvpAvatar _avatar;
        readonly AtlasNetworkManager _net;
        readonly Transform _target;
        readonly Func<Vector2>? _joystickSource;
        readonly Func<bool>? _fireRequestSource;
        readonly Func<float>? _cameraYawSource;
        readonly Action<AtlasVec> _respawnHandler;
        Vector3 _localPos;
        Vector3 _localDir = Vector3.forward;
        float _reportAccum;

        public PlayerInputController(MvpAvatar avatar, AtlasNetworkManager net, Transform target,
            Func<Vector2>? joystickSource = null, Func<bool>? fireRequestSource = null,
            Func<float>? cameraYawSource = null)
        {
            _avatar = avatar;
            _net = net;
            _target = target;
            _joystickSource = joystickSource;
            _fireRequestSource = fireRequestSource;
            _cameraYawSource = cameraYawSource;
            _localPos = new Vector3(_avatar.Position.X, _avatar.Position.Y, _avatar.Position.Z);
            // Snap local sim to the server's spawn pos so the dead capsule
            // doesn't keep walking under WASD between death and respawn.
            _respawnHandler = pos =>
                _localPos = new Vector3(pos.X, pos.Y, pos.Z);
            _avatar.Respawned += _respawnHandler;
            Ticker.Add(this);
        }

        public void Dispose()
        {
            Ticker.Remove(this);
            _avatar.Respawned -= _respawnHandler;
        }

        public void Tick(float dt)
        {
            if (_avatar == null || _avatar.IsDestroyed) return;
            if (_avatar.IsDead) return;

            float h = Input.GetAxisRaw("Horizontal");
            float v = Input.GetAxisRaw("Vertical");
            // Joystick takes priority when active so finger drag doesn't
            // fight a sticky keyboard axis from a held WASD key.
            var joy = _joystickSource?.Invoke() ?? Vector2.zero;
            var input = joy.sqrMagnitude > 0.01f
                ? new Vector3(joy.x, 0f, joy.y)
                : new Vector3(h, 0f, v);
            // Avatar faces camera forward (not move direction) so strafing
            // doesn't feed back into the camera yaw-follow loop.
            float camYaw = _cameraYawSource?.Invoke() ?? 0f;
            var camRot = Quaternion.Euler(0f, camYaw, 0f);
            bool moving = input.sqrMagnitude > 0.01f;
            if (moving)
                _localPos += camRot * Vector3.ClampMagnitude(input, 1f) * (kMoveSpeed * dt);

            float curYaw = Mathf.Atan2(_localDir.x, _localDir.z) * Mathf.Rad2Deg;
            float yawDiff = Mathf.DeltaAngle(curYaw, camYaw);
            float turnRate = moving
                ? kMoveTurnRate
                : (Mathf.Abs(yawDiff) > kIdleTurnThresholdDeg ? kIdleTurnRate : 0f);
            if (turnRate > 0f)
            {
                float newYaw = Mathf.MoveTowardsAngle(curYaw, camYaw, turnRate * dt);
                _localDir = Quaternion.Euler(0f, newYaw, 0f) * Vector3.forward;
            }

            // Owner is client-authoritative; render directly from _localPos.
            _target.position = _localPos;
            if (_localDir.sqrMagnitude > 0.01f)
                _target.rotation = Quaternion.LookRotation(_localDir);

            _reportAccum += dt;
            if (_reportAccum >= 1f / kReportHz)
            {
                _reportAccum = 0f;
                _avatar.Cell.ReportPos(_localPos.ToAtlas(), _localDir.ToAtlas());
            }

            bool fire = Input.GetKeyDown(KeyCode.Space)
                || (_fireRequestSource?.Invoke() ?? false);
            if (fire)
                _avatar.Cell.LaunchProjectile(_localDir.ToAtlas());
        }
    }
}
