using System;
using Atlas.Client;
using Atlas.Client.Native;
using Atlas.Client.Unity;
using UnityEngine;
using AtlasVec = Atlas.DataTypes.Vector3;
using MvpAvatar = Atlas.Mvp.Client.Avatar;

namespace Atlas.Mvp.Unity
{
    public sealed class PlayerInputController : IAtlasUnityTickable, IDisposable
    {
        const float kInputHz = 30f;
        const ushort kInputDtMs = 33;

        readonly MvpAvatar _avatar;
        readonly AtlasNetworkManager _net;
        readonly ClientSession _session;
        readonly Transform _target;
        readonly AtlasUnityFramePump _frame;
        readonly Func<Vector2>? _joystickSource;
        readonly Func<bool>? _fireRequestSource;
        readonly Func<float>? _cameraYawSource;
        readonly Func<bool>? _inputBlocked;
        readonly Action<AtlasVec> _respawnHandler;
        readonly Action<MovementStateAck> _ackHandler;
        readonly Action<MovementCommandStart> _commandStartHandler;
        readonly Action<MovementCommandEnd> _commandEndHandler;
        readonly UnityOwnerMovementPredictor _predictor = new();
        readonly AtlasMovementInputFrame[] _sendFrames = new AtlasMovementInputFrame[3];
        Vector3 _aimDir = Vector3.forward;
        float _inputAccum;
        uint _nextInputSeq = 1;
        uint _inputTick = 1;

        public PlayerInputController(MvpAvatar avatar, AtlasNetworkManager net, Transform target,
            AtlasUnityFramePump frame, Func<Vector2>? joystickSource = null,
            Func<bool>? fireRequestSource = null,
            Func<float>? cameraYawSource = null, Func<bool>? inputBlocked = null)
        {
            _avatar = avatar;
            _net = net;
            _session = net.Session;
            _target = target;
            _frame = frame;
            _joystickSource = joystickSource;
            _fireRequestSource = fireRequestSource;
            _cameraYawSource = cameraYawSource;
            _inputBlocked = inputBlocked;
            var spawn = new Vector3(_avatar.Position.X, _avatar.Position.Y, _avatar.Position.Z);
            _predictor.Reset(spawn, _aimDir);
            _respawnHandler = pos =>
            {
                var p = new Vector3(pos.X, pos.Y, pos.Z);
                _predictor.Reset(p, _aimDir);
                _target.position = p;
            };
            _ackHandler = OnMovementStateAck;
            _commandStartHandler = OnMovementCommandStart;
            _commandEndHandler = OnMovementCommandEnd;
            _avatar.Respawned += _respawnHandler;
            _session.MovementStateAckReceived += _ackHandler;
            _session.MovementCommandStarted += _commandStartHandler;
            _session.MovementCommandEnded += _commandEndHandler;
            _frame.Add(this);
        }

        public void Dispose()
        {
            _frame.Remove(this);
            _avatar.Respawned -= _respawnHandler;
            _session.MovementStateAckReceived -= _ackHandler;
            _session.MovementCommandStarted -= _commandStartHandler;
            _session.MovementCommandEnded -= _commandEndHandler;
        }

        public void Tick(float dt)
        {
            if (_avatar == null || _avatar.IsDestroyed) return;
            _predictor.TickVisualOffset(dt);
            if (_avatar.IsDead) return;

            // Suppress raw kbd polling while chat input has focus, otherwise
            // typing W/A/S/D/Space keeps moving + firing the Avatar.
            bool blocked = _inputBlocked?.Invoke() ?? false;
            float h = blocked ? 0f : Input.GetAxisRaw("Horizontal");
            float v = blocked ? 0f : Input.GetAxisRaw("Vertical");
            // Joystick takes priority when active so finger drag doesn't
            // fight a sticky keyboard axis from a held WASD key.
            var joy = _joystickSource?.Invoke() ?? Vector2.zero;
            var input = joy.sqrMagnitude > 0.01f
                ? new Vector3(joy.x, 0f, joy.y)
                : new Vector3(h, 0f, v);
            input = Vector3.ClampMagnitude(input, 1f);
            float camYaw = _cameraYawSource?.Invoke() ?? 0f;
            _aimDir = Quaternion.Euler(0f, camYaw, 0f) * Vector3.forward;

            _inputAccum += dt;
            bool pushedInput = false;
            while (_inputAccum >= 1f / kInputHz)
            {
                _inputAccum -= 1f / kInputHz;
                if (!_predictor.AcceptsInput) continue;
                pushedInput |= _predictor.PushInput(BuildInputFrame(input, camYaw));
            }

            if (pushedInput)
            {
                int count = _predictor.CopyRecentFrames(_sendFrames);
                if (count > 0)
                    _net.SendMovementInput(_avatar.EntityId,
                        new ReadOnlySpan<AtlasMovementInputFrame>(_sendFrames, 0, count));
            }

            _target.position = _predictor.RenderPosition;
            var renderDir = _predictor.RenderDirection;
            if (renderDir.sqrMagnitude > 0.01f)
                _target.rotation = Quaternion.LookRotation(renderDir);

            bool fire = (!blocked && Input.GetKeyDown(KeyCode.Space))
                || (_fireRequestSource?.Invoke() ?? false);
            if (fire)
                _avatar.Cell.LaunchProjectile(_aimDir.ToAtlas());

            if (!blocked && Input.GetKeyDown(KeyCode.LeftShift))
                _avatar.Cell.Dash(_aimDir.ToAtlas());

            if (!blocked)
            {
                if (Input.GetKeyDown(KeyCode.Alpha1)) _avatar.Equipment?.EquipWeapon(1);
                else if (Input.GetKeyDown(KeyCode.Alpha2)) _avatar.Equipment?.EquipWeapon(2);
                else if (Input.GetKeyDown(KeyCode.Alpha3)) _avatar.Equipment?.EquipWeapon(3);
            }
        }

        AtlasMovementInputFrame BuildInputFrame(Vector3 move, float camYaw)
        {
            return new AtlasMovementInputFrame
            {
                Seq = _nextInputSeq++,
                InputTick = _inputTick++,
                MoveX = QuantizeAxis(move.x),
                MoveZ = QuantizeAxis(move.z),
                ViewYaw = QuantizeYaw(camYaw),
                ViewPitch = 0,
                Buttons = 0,
                ClientDtMs = kInputDtMs,
            };
        }

        void OnMovementStateAck(MovementStateAck ack)
        {
            if (ack.EntityId != _avatar.EntityId) return;
            if (_predictor.ApplyAck(ack, out float distanceM, out ushort correctionFlags))
            {
                _nextInputSeq = MovementSequence.SeedNextInputSeqFromAck(
                    _nextInputSeq, ack.AckedInputSeq);
                _net.SendMovementCorrectionReport(ack.EntityId, ack.AckedInputSeq,
                    ack.ServerTick, distanceM, correctionFlags);
            }
        }

        void OnMovementCommandStart(MovementCommandStart commandStart)
        {
            if (commandStart.EntityId != _avatar.EntityId) return;
            if (_predictor.ApplyCommandStart(commandStart))
                _target.position = _predictor.RenderPosition;
        }

        void OnMovementCommandEnd(MovementCommandEnd commandEnd)
        {
            if (commandEnd.EntityId != _avatar.EntityId) return;
            if (_predictor.ApplyCommandEnd(commandEnd))
            {
                _nextInputSeq = MovementSequence.SeedNextInputSeqFromAck(
                    _nextInputSeq, commandEnd.State.LastProcessedInputSeq);
                _target.position = _predictor.RenderPosition;
            }
        }

        static sbyte QuantizeAxis(float value) =>
            (sbyte)Mathf.RoundToInt(Mathf.Clamp(value, -1f, 1f) * 127f);

        static ushort QuantizeYaw(float degrees)
        {
            float normalized = Mathf.Repeat(degrees, 360f) / 360f;
            return (ushort)Mathf.RoundToInt(normalized * 65535f);
        }
    }
}
