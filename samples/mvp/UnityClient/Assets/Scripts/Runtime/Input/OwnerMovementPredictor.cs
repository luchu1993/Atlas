using Atlas.Client;
using Atlas.Client.Native;
using Atlas.Client.Unity;
using UnityEngine;
using AtlasPredictor = Atlas.Client.OwnerMovementPredictor;

namespace Atlas.Mvp.Unity
{
    sealed class UnityOwnerMovementPredictor
    {
        readonly AtlasPredictor _inner = new(PredictMovement);

        public Vector3 RenderPosition => _inner.RenderPosition.ToUnity();
        public Vector3 RenderDirection => _inner.RenderDirection.ToUnity();
        public bool AcceptsInput => _inner.AcceptsInput;

        public void Reset(Vector3 position, Vector3 direction)
        {
            _inner.Reset(position.ToAtlas(), direction.ToAtlas());
        }

        public bool PushInput(AtlasMovementInputFrame input) =>
            _inner.PushInput(input);

        public bool ApplyAck(MovementStateAck ack, out float correctionDistanceM,
                             out ushort correctionFlags) =>
            _inner.ApplyAck(ack, out correctionDistanceM, out correctionFlags);

        public bool ApplyCommandStart(MovementCommandStart commandStart) =>
            _inner.ApplyCommandStart(commandStart);

        public bool ApplyCommandEnd(MovementCommandEnd commandEnd) =>
            _inner.ApplyCommandEnd(commandEnd);

        public void TickVisualOffset(float dt) =>
            _inner.TickVisualOffset(dt);

        public int CopyRecentFrames(AtlasMovementInputFrame[] destination) =>
            _inner.CopyRecentFrames(destination);

        static bool PredictMovement(AtlasMovementStateFrame previous,
                                    AtlasMovementInputFrame input,
                                    uint serverTick,
                                    out AtlasMovementStateFrame next)
        {
            int rc = AtlasNetworkManager.PredictMovement(previous, input, serverTick, out next);
            return rc == AtlasNetReturnCode.Ok;
        }
    }
}
