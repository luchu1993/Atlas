using System;
using Atlas.Client;
using Atlas.Client.Native;
using Atlas.Components;
using Atlas.Diagnostics;

namespace Atlas.ClientSample;

public sealed class StressMovementBotComponent : ClientLocalComponent
{
    const float kSendIntervalS = 0.1f;
    const float kYawRateRadS = 0.35f;
    const int kHistoryCapacity = 16;
    const int kSendFrameCapacity = 3;

    readonly OwnerMovementPredictor _predictor = new(PredictStep, kHistoryCapacity);
    readonly AtlasMovementInputFrame[] _sendFrames =
        new AtlasMovementInputFrame[kSendFrameCapacity];
    float _accumS;
    float _yawRadians;
    uint _nextSeq = 1;
    uint _inputTick;
    bool _subscribed;

    StressAvatar Avatar => (StressAvatar)Entity;

    public override void OnAttached()
    {
        _predictor.Reset(Avatar.Position, Avatar.Direction);
        ClientCallbacks.MovementStateAckReceived += OnMovementStateAck;
        ClientCallbacks.MovementCommandStarted += OnMovementCommandStart;
        ClientCallbacks.MovementCommandEnded += OnMovementCommandEnd;
        _subscribed = true;
    }

    public override void OnDetached()
    {
        if (!_subscribed) return;
        ClientCallbacks.MovementStateAckReceived -= OnMovementStateAck;
        ClientCallbacks.MovementCommandStarted -= OnMovementCommandStart;
        ClientCallbacks.MovementCommandEnded -= OnMovementCommandEnd;
        _subscribed = false;
    }

    public override void OnTick(float deltaTime)
    {
        if (Entity.IsDestroyed) return;
        _predictor.TickVisualOffset(deltaTime);
        _accumS += MathF.Max(0.0f, deltaTime);
        if (_accumS < kSendIntervalS) return;
        _accumS = MathF.Min(_accumS - kSendIntervalS, kSendIntervalS);
        SendInputFrame();
    }

    void SendInputFrame()
    {
        if (!_predictor.AcceptsInput) return;
        _yawRadians += kYawRateRadS * kSendIntervalS;
        var frame = new AtlasMovementInputFrame
        {
            Seq = _nextSeq++,
            InputTick = ++_inputTick,
            MoveZ = 96,
            ViewYaw = EncodeYaw(_yawRadians),
            ClientDtMs = (ushort)Math.Clamp((int)(kSendIntervalS * 1000.0f), 1, 250),
        };

        if (!_predictor.PushInput(frame)) return;
        int count = _predictor.CopyRecentFrames(_sendFrames);
        Avatar.SendMovementFrames(new ReadOnlySpan<AtlasMovementInputFrame>(_sendFrames, 0, count));
        Log.Info($"[StressAvatar:{Entity.EntityId}] OnMovementInputSent "
                 + $"seq={frame.Seq} tick={frame.InputTick} count={count}");
    }

    void OnMovementStateAck(MovementStateAck ack)
    {
        if (ack.EntityId != Entity.EntityId) return;
        if (!_predictor.ApplyAck(ack, out float distanceM, out ushort correctionFlags)) return;
        _nextSeq = MovementSequence.SeedNextInputSeqFromAck(_nextSeq, ack.AckedInputSeq);
        var tier = MovementCorrection.Classify(distanceM);
        Log.Info($"[StressAvatar:{Entity.EntityId}] OnMovementCorrection "
                 + $"ack={ack.AckedInputSeq} tick={ack.ServerTick} "
                 + $"tier={(ushort)tier} distance={distanceM:F3}");
        Avatar.SendMovementCorrection(ack.AckedInputSeq, ack.ServerTick,
                                      distanceM, correctionFlags);
        Log.Info($"[StressAvatar:{Entity.EntityId}] OnMovementCorrectionReportSent "
                 + $"ack={ack.AckedInputSeq} tick={ack.ServerTick} "
                 + $"flags={correctionFlags} distance={distanceM:F3}");
    }

    void OnMovementCommandStart(MovementCommandStart commandStart)
    {
        if (commandStart.EntityId != Entity.EntityId) return;
        _predictor.ApplyCommandStart(commandStart);
    }

    void OnMovementCommandEnd(MovementCommandEnd commandEnd)
    {
        if (commandEnd.EntityId != Entity.EntityId) return;
        _predictor.ApplyCommandEnd(commandEnd);
    }

    static unsafe bool PredictStep(AtlasMovementStateFrame previous,
                                   AtlasMovementInputFrame input,
                                   uint serverTick,
                                   out AtlasMovementStateFrame next)
    {
        AtlasMovementStateFrame nextValue;
        int rc = AtlasNetNative.AtlasNetMovementPredictStep(&previous, &input, serverTick,
                                                            &nextValue);
        next = nextValue;
        return rc == AtlasNetReturnCode.Ok;
    }

    static ushort EncodeYaw(float radians)
    {
        const float twoPi = MathF.PI * 2.0f;
        float normalized = radians % twoPi;
        if (normalized < 0.0f) normalized += twoPi;
        return (ushort)MathF.Round(normalized / twoPi * ushort.MaxValue);
    }
}
