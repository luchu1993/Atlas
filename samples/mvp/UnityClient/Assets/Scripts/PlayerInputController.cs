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

        readonly MvpAvatar _avatar;
        readonly AtlasNetworkManager _net;
        readonly Transform _target;
        readonly Action<AtlasVec> _respawnHandler;
        Vector3 _localPos;
        Vector3 _localDir = Vector3.forward;
        float _reportAccum;

        public PlayerInputController(MvpAvatar avatar, AtlasNetworkManager net, Transform target)
        {
            _avatar = avatar;
            _net = net;
            _target = target;
            _localPos = new Vector3(_avatar.Position.X, _avatar.Position.Y, _avatar.Position.Z);
            // Snap local sim to the server's spawn pos so the dead capsule
            // doesn't keep walking under WASD between death and respawn.
            _respawnHandler = pos =>
                _localPos = new Vector3(pos.X, pos.Y, pos.Z);
            _avatar.Respawned += _respawnHandler;
            Tick.Add(this);
        }

        public void Dispose()
        {
            Tick.Remove(this);
            _avatar.Respawned -= _respawnHandler;
        }

        public void Tick(float dt)
        {
            if (_avatar == null || _avatar.IsDestroyed) return;
            if (_avatar.IsDead) return;

            float h = Input.GetAxisRaw("Horizontal");
            float v = Input.GetAxisRaw("Vertical");
            var input = new Vector3(h, 0f, v);
            if (input.sqrMagnitude > 0.01f)
            {
                // Avatar faces camera forward (not move direction) so strafing
                // doesn't feed back into the camera yaw-follow loop.
                var camYaw = Bootstrap.Instance?.CameraYaw ?? 0f;
                var camRot = Quaternion.Euler(0f, camYaw, 0f);
                _localPos += camRot * input.normalized * (kMoveSpeed * dt);
                _localDir = camRot * Vector3.forward;
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

            if (Input.GetKeyDown(KeyCode.Space))
                _avatar.Cell.LaunchProjectile(_localDir.ToAtlas());
        }
    }
}
