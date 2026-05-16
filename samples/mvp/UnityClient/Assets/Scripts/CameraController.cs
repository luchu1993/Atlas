using System;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public sealed class CameraController : ITickable, ILateTickable, IDisposable
    {
        public struct Config
        {
            public Vector3 FollowOffset;
            public float LookHeight;
            public float PositionSmoothTime;
            public float YawFollowSmoothTime;
            public float MouseSensitivity;
            public float HudLookSensitivity;
            public float PitchMin, PitchMax, InitialPitch;
            public float ZoomMin, ZoomMax, ZoomStep;
        }

        readonly Camera _camera;
        readonly Config _cfg;
        readonly Func<Vector2>? _hudLookDelta;
        readonly Func<bool>? _moveInputActive;
        Transform? _followTarget;
        float _yaw, _pitch;
        float _zoom = 1f;
        Vector3 _camVelocity;
        float _yawVelocity;
        bool _initialized;

        public Camera Camera => _camera;
        public float Yaw => _yaw;

        public CameraController(Camera camera, Config cfg,
            Func<Vector2>? hudLookDelta = null, Func<bool>? moveInputActive = null)
        {
            _camera = camera;
            _cfg = cfg;
            _hudLookDelta = hudLookDelta;
            _moveInputActive = moveInputActive;
            ApplyBootPose();
            Ticker.Add(this);
        }

        public void Dispose() => Ticker.Remove(this);

        public void SetFollowTarget(Transform? target)
        {
            _followTarget = target;
            _initialized = false;
            _zoom = 1f;
            _camVelocity = Vector3.zero;
            _yawVelocity = 0f;
            if (target != null) ApplyInWorldPose();
            else ApplyBootPose();
        }

        public void Tick(float dt)
        {
            if (_followTarget == null) return;
            float scroll = Input.mouseScrollDelta.y;
            if (Mathf.Abs(scroll) > 0.01f)
                _zoom = Mathf.Clamp(_zoom * Mathf.Pow(1f - _cfg.ZoomStep, scroll),
                    _cfg.ZoomMin, _cfg.ZoomMax);
        }

        public void LateTick()
        {
            if (_followTarget == null) return;
            var targetPos = _followTarget.position;
            float ownerYaw = _followTarget.rotation.eulerAngles.y;

            if (!_initialized)
            {
                _yaw = ownerYaw;
                _pitch = _cfg.InitialPitch;
                _initialized = true;
            }

            if (Input.GetMouseButton(1))
            {
                Cursor.lockState = CursorLockMode.Locked;
                _yaw += Input.GetAxis("Mouse X") * _cfg.MouseSensitivity;
                _pitch -= Input.GetAxis("Mouse Y") * _cfg.MouseSensitivity;
                _pitch = Mathf.Clamp(_pitch, _cfg.PitchMin, _cfg.PitchMax);
            }
            else
            {
                Cursor.lockState = CursorLockMode.None;
                var look = _hudLookDelta?.Invoke() ?? Vector2.zero;
                if (look != Vector2.zero)
                {
                    // UI Y grows downward; mirror the Mouse Y sign convention
                    // so drag-up tilts the camera up.
                    _yaw += look.x * _cfg.HudLookSensitivity;
                    _pitch += look.y * _cfg.HudLookSensitivity;
                    _pitch = Mathf.Clamp(_pitch, _cfg.PitchMin, _cfg.PitchMax);
                }
                else if (_moveInputActive?.Invoke() == true)
                {
                    _yaw = Mathf.SmoothDampAngle(_yaw, ownerYaw, ref _yawVelocity,
                        _cfg.YawFollowSmoothTime);
                }
            }

            var orbitRot = Quaternion.Euler(_pitch, _yaw, 0f);
            var desiredPos = targetPos
                + Vector3.up * (_cfg.FollowOffset.y * _zoom)
                + orbitRot * Vector3.back * (Mathf.Abs(_cfg.FollowOffset.z) * _zoom);
            _camera.transform.position = Vector3.SmoothDamp(
                _camera.transform.position, desiredPos, ref _camVelocity, _cfg.PositionSmoothTime);
            _camera.transform.LookAt(targetPos + Vector3.up * _cfg.LookHeight);
        }

        void ApplyBootPose()
        {
            _camera.backgroundColor = new Color(0.05f, 0.05f, 0.07f);
            _camera.clearFlags = CameraClearFlags.SolidColor;
            _camera.transform.position = new Vector3(0f, 5f, -8f);
            _camera.transform.LookAt(Vector3.zero);
        }

        void ApplyInWorldPose()
        {
            _camera.backgroundColor = new Color(0.5f, 0.6f, 0.7f);
            _camera.transform.position = new Vector3(0f, 5f, -8f);
            _camera.transform.LookAt(Vector3.zero);
        }
    }
}
