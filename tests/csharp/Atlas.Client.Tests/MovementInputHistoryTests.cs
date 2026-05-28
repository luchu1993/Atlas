using Atlas.Client.Native;
using Atlas.DataTypes;
using Atlas.Shared.Protocol;
using Xunit;

namespace Atlas.Client.Tests;

public sealed class MovementInputHistoryTests
{
    [Fact]
    public void MovementSequenceHandlesWrap()
    {
        Assert.True(MovementSequence.IsNewer(2, 1));
        Assert.False(MovementSequence.IsNewer(1, 1));
        Assert.False(MovementSequence.IsNewer(1, 2));
        Assert.True(MovementSequence.IsNewer(0, uint.MaxValue));
        Assert.False(MovementSequence.IsNewer(uint.MaxValue, 0));
        Assert.Equal(2u, MovementSequence.Delta(3, 1));
        Assert.Equal(256u, MovementSequence.MaxInputSequenceGap);
    }

    [Fact]
    public void MovementSequenceDetectsStaleAck()
    {
        Assert.False(MovementSequence.IsAckStale(1, 0, hasLastAck: false));
        Assert.False(MovementSequence.IsAckStale(0, uint.MaxValue, hasLastAck: true));
        Assert.False(MovementSequence.IsAckStale(42, 42, hasLastAck: true));
        Assert.True(MovementSequence.IsAckStale(uint.MaxValue, 0, hasLastAck: true));

        Assert.False(MovementSequence.IsAckStale(42, 100, 42, 90, hasLastAck: true));
        Assert.True(MovementSequence.IsAckStale(42, 90, 42, 100, hasLastAck: true));
        Assert.True(MovementSequence.IsAckStale(42, 100, 42, 100, hasLastAck: true));
        Assert.False(MovementSequence.IsAckStale(43, 1, 42, 100, hasLastAck: true));
    }

    [Fact]
    public void MovementSequenceSeedsNextInputFromAck()
    {
        Assert.Equal(1001u, MovementSequence.SeedNextInputSeqFromAck(2, 1000));
        Assert.Equal(43u, MovementSequence.SeedNextInputSeqFromAck(43, 41));
        Assert.Equal(0u,
            MovementSequence.SeedNextInputSeqFromAck(uint.MaxValue, uint.MaxValue));
    }

    [Fact]
    public void MovementCorrectionClassifiesSharedThresholds()
    {
        Assert.Equal(MovementCorrectionTier.None, MovementCorrection.Classify(0.29f));
        Assert.Equal(MovementCorrectionTier.Tier1,
            MovementCorrection.Classify(MovementCorrection.Tier1DistanceM));
        Assert.Equal(MovementCorrectionTier.Tier2,
            MovementCorrection.Classify(MovementCorrection.Tier2DistanceM));
        Assert.Equal(MovementCorrectionTier.Snap,
            MovementCorrection.Classify(MovementCorrection.SnapDistanceM));
        Assert.Equal(MovementCorrection.Tier1Flag,
            MovementCorrection.FlagFor(MovementCorrectionTier.Tier1));
        Assert.Equal(MovementCorrection.Tier2Flag,
            MovementCorrection.FlagFor(MovementCorrectionTier.Tier2));
        Assert.Equal(MovementCorrection.SnapFlag,
            MovementCorrection.FlagFor(MovementCorrectionTier.Snap));
    }

    [Fact]
    public void MovementCurvesSampleRegisteredCurve()
    {
        Assert.True(MovementCurves.Register(77, new[] { 0.0f, 0.0f, 1.0f }));

        Assert.True(MovementCurves.Contains(77));
        Assert.Equal(0.0f, MovementCurves.Sample(77, 0.5f), 3);
        Assert.Equal(0.5f, MovementCurves.Sample(77, 0.75f), 3);
        Assert.False(MovementCurves.Register(78, new[] { 0.0f, float.NaN }));
    }

    [Fact]
    public void CopyRecentReturnsNewestFramesInOrder()
    {
        var history = new MovementInputHistory(8);
        for (uint seq = 1; seq <= 5; ++seq)
            history.Push(Frame(seq));

        var recent = new AtlasMovementInputFrame[3];
        int count = history.CopyRecent(recent);

        Assert.Equal(3, count);
        Assert.Equal(3u, recent[0].Seq);
        Assert.Equal(4u, recent[1].Seq);
        Assert.Equal(5u, recent[2].Seq);
    }

    [Fact]
    public void DropAckedKeepsOnlyUnconfirmedFrames()
    {
        var history = new MovementInputHistory(8);
        for (uint seq = 10; seq <= 14; ++seq)
            history.Push(Frame(seq));

        history.DropAcked(12);

        Assert.Equal(2, history.Count);
        Assert.Equal(13u, history[0].Seq);
        Assert.Equal(14u, history[1].Seq);
    }

    [Fact]
    public void CapacityDropsOldestFrames()
    {
        var history = new MovementInputHistory(3);
        for (uint seq = 1; seq <= 5; ++seq)
            history.Push(Frame(seq));

        Assert.Equal(3, history.Count);
        Assert.Equal(3u, history[0].Seq);
        Assert.Equal(4u, history[1].Seq);
        Assert.Equal(5u, history[2].Seq);
    }

    [Fact]
    public void DropAckedHandlesSequenceWrap()
    {
        var history = new MovementInputHistory(8);
        history.Push(Frame(uint.MaxValue - 1));
        history.Push(Frame(uint.MaxValue));
        history.Push(Frame(0));
        history.Push(Frame(1));

        history.DropAcked(uint.MaxValue - 1);

        Assert.Equal(3, history.Count);
        Assert.Equal(uint.MaxValue, history[0].Seq);
        Assert.Equal(0u, history[1].Seq);
        Assert.Equal(1u, history[2].Seq);

        history.DropAcked(0);

        Assert.Equal(1, history.Count);
        Assert.Equal(1u, history[0].Seq);
    }

    [Fact]
    public void OwnerPredictorReplaysPendingInputsAfterAck()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);

        Assert.True(predictor.PushInput(Frame(1, 100)));
        Assert.True(predictor.PushInput(Frame(2, 100)));

        var state = new MovementState(new Vector3(0.0f, 0.0f, 0.5f),
                                      Vector3.Zero, Vector3.Forward, 1, 1);
        var ack = new MovementStateAck(1, 1, 10, state, 0);

        Assert.True(predictor.ApplyAck(ack, out float distanceM, out ushort flags));

        Assert.Equal(1, predictor.PendingInputCount);
        Assert.Equal(0.5f, distanceM, 3);
        Assert.Equal(MovementCorrection.Tier1Flag, flags);
        Assert.Equal(1.5f, predictor.PredictedPosition.Z, 3);
        Assert.Equal(2.0f, predictor.RenderPosition.Z, 3);
        Assert.Equal(0.5f, predictor.VisualOffset.Z, 3);
        Assert.False(predictor.ApplyAck(ack, out _, out _));
    }

    [Fact]
    public void OwnerPredictorAppliesMovementCommandStart()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);

        Assert.True(predictor.ApplyCommandStart(CommandStart(elapsedMs: 250)));

        Assert.True(predictor.HasActiveCommand);
        Assert.Equal(1.0f, predictor.PredictedPosition.Z, 3);
        Assert.Equal(1.0f, predictor.RenderDirection.Z, 3);
        Assert.False(predictor.PushInput(Frame(1, 100)));
        predictor.TickVisualOffset(0.25f);
        Assert.Equal(2.0f, predictor.PredictedPosition.Z, 3);
        predictor.TickVisualOffset(0.5f);
        Assert.Equal(4.0f, predictor.PredictedPosition.Z, 3);
        Assert.False(predictor.HasActiveCommand);
    }

    [Fact]
    public void OwnerPredictorRejectsStaleMovementCommandStartAfterAck()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        var state = new MovementState(new Vector3(0.0f, 0.0f, 3.0f),
                                      Vector3.Zero, Vector3.Forward, 1, 5);
        var ack = new MovementStateAck(1, 5, 30, state, 0);
        Assert.True(predictor.ApplyAck(ack, out _, out _));

        Assert.False(predictor.ApplyCommandStart(CommandStart(serverTick: 20)));
        Assert.False(predictor.HasActiveCommand);
        Assert.Equal(3.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorRejectsStaleMovementCommandStartBehindNewerStart()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);

        Assert.True(predictor.ApplyCommandStart(CommandStart(commandId: 1,
                                                             serverTick: 20)));
        Assert.True(predictor.ApplyCommandStart(CommandStart(commandId: 2,
                                                             serverTick: 20,
                                                             priority: 1)));
        Assert.False(predictor.ApplyCommandStart(CommandStart(commandId: 1,
                                                              serverTick: 20)));
        Assert.False(predictor.ApplyCommandStart(CommandStart(commandId: 2,
                                                              serverTick: 20,
                                                              priority: 1)));
        Assert.True(predictor.HasActiveCommand);
    }

    [Fact]
    public void OwnerPredictorRejectsAckOlderThanMovementCommandStart()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart(serverTick: 20)));
        var state = new MovementState(new Vector3(0.0f, 0.0f, 3.0f),
                                      Vector3.Zero, Vector3.Forward, 1, 5);
        var ack = new MovementStateAck(1, 5, 10, state, 0);

        Assert.False(predictor.ApplyAck(ack, out _, out _));
        Assert.True(predictor.HasActiveCommand);
        Assert.Equal(0.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorRejectsCommandEndOlderThanMovementCommandStart()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart(serverTick: 20)));
        var state = new MovementState(new Vector3(0.0f, 0.0f, 3.0f),
                                      Vector3.Zero, Vector3.Forward, 1, 5);

        Assert.False(predictor.ApplyCommandEnd(new MovementCommandEnd(1, 1, 10, state)));
        Assert.True(predictor.HasActiveCommand);
        Assert.Equal(0.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorRejectsZeroMovementCommandEnd()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart()));
        var state = new MovementState(new Vector3(0.0f, 0.0f, 3.0f),
                                      Vector3.Zero, Vector3.Forward, 1, 5);

        Assert.False(predictor.ApplyCommandEnd(new MovementCommandEnd(1, 0, 20, state)));
        Assert.True(predictor.HasActiveCommand);
        Assert.Equal(0.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorAcceptsSameTickCommandStartAfterCommandEnd()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart(commandId: 1,
                                                             serverTick: 20,
                                                             priority: 2)));
        var state = new MovementState(new Vector3(0.0f, 0.0f, 1.0f),
                                      Vector3.Zero, Vector3.Forward, 1, 0);
        Assert.True(predictor.ApplyCommandEnd(new MovementCommandEnd(1, 1, 20, state)));

        Assert.True(predictor.ApplyCommandStart(CommandStart(commandId: 2,
                                                             serverTick: 20,
                                                             priority: 0)));
        Assert.True(predictor.HasActiveCommand);
        Assert.Equal(0.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorAppliesMovementCommandCurve()
    {
        Assert.True(MovementCurves.Register(79, new[] { 0.0f, 0.0f, 1.0f }));
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);

        Assert.True(predictor.ApplyCommandStart(CommandStart(curveId: 79)));

        predictor.TickVisualOffset(0.5f);
        Assert.Equal(0.0f, predictor.PredictedPosition.Z, 3);
        predictor.TickVisualOffset(0.25f);
        Assert.Equal(2.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorAllowTurnCommandConsumesInputForDirectionOnly()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart(
            inputPolicy: MovementCommandInputPolicy.AllowTurn)));

        Assert.True(predictor.PushInput(new AtlasMovementInputFrame
        {
            Seq = 1,
            InputTick = 101,
            MoveZ = 100,
            ViewYaw = 16384,
            ClientDtMs = 33,
        }));

        Assert.Equal(1, predictor.PendingInputCount);
        Assert.Equal(0.0f, predictor.PredictedPosition.Z, 3);
        Assert.Equal(1.0f, predictor.RenderDirection.X, 3);

        predictor.TickVisualOffset(0.25f);

        Assert.Equal(1.0f, predictor.PredictedPosition.Z, 3);
        Assert.Equal(1.0f, predictor.RenderDirection.X, 3);
    }

    [Fact]
    public void OwnerPredictorAcceptsInputReflectsMovementCommandPolicy()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);

        Assert.True(predictor.AcceptsInput);
        Assert.True(predictor.ApplyCommandStart(CommandStart()));
        Assert.False(predictor.AcceptsInput);
        predictor.TickVisualOffset(1.0f);
        Assert.False(predictor.HasActiveCommand);
        Assert.False(predictor.AcceptsInput);
        Assert.False(predictor.PushInput(Frame(1, 100)));
        Assert.Equal(0, predictor.PendingInputCount);

        var state = new MovementState(Vector3.Zero, Vector3.Zero, Vector3.Forward, 1, 0);
        Assert.True(predictor.ApplyCommandEnd(new MovementCommandEnd(1, 1, 20, state)));
        Assert.True(predictor.AcceptsInput);

        Assert.True(predictor.ApplyCommandStart(CommandStart(
            commandId: 2, serverTick: 20,
            inputPolicy: MovementCommandInputPolicy.AllowTurn)));
        predictor.TickVisualOffset(1.0f);
        Assert.False(predictor.HasActiveCommand);
        Assert.True(predictor.AcceptsInput);
        Assert.True(predictor.PushInput(Frame(2, 100)));
    }

    [Fact]
    public void OwnerPredictorClearsPendingInputsOnMovementCommandStart()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.PushInput(Frame(1, 100)));
        Assert.True(predictor.PushInput(Frame(2, 100)));

        Assert.True(predictor.ApplyCommandStart(CommandStart()));

        Assert.Equal(0, predictor.PendingInputCount);
    }

    [Fact]
    public void OwnerPredictorAlignsActiveCommandToAckState()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart()));
        var state = new MovementState(new Vector3(0.0f, 0.0f, 2.0f),
                                      Vector3.Zero, Vector3.Forward, 1, 0);
        var ack = new MovementStateAck(1, 0, 20, state, 0);

        Assert.True(predictor.ApplyAck(ack, out _, out _));
        predictor.TickVisualOffset(0.1f);

        Assert.Equal(2.4f, predictor.PredictedPosition.Z, 3);
        Assert.True(predictor.HasActiveCommand);
    }

    [Fact]
    public void OwnerPredictorAppliesMovementCommandEndAndReplaysPendingInput()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart(
            inputPolicy: MovementCommandInputPolicy.AllowTurn)));
        predictor.TickVisualOffset(1.0f);
        Assert.True(predictor.PushInput(Frame(2, 100)));
        var state = new MovementState(new Vector3(0.0f, 0.0f, 3.0f),
                                      Vector3.Zero, Vector3.Forward, 1, 1);

        Assert.True(predictor.ApplyCommandEnd(new MovementCommandEnd(1, 1, 20, state)));

        Assert.False(predictor.HasActiveCommand);
        Assert.Equal(1, predictor.PendingInputCount);
        Assert.Equal(4.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorCommandEndAdvancesAckCursor()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart()));
        var endState = new MovementState(new Vector3(0.0f, 0.0f, 3.0f),
                                         Vector3.Zero, Vector3.Forward, 1, 5);

        Assert.True(predictor.ApplyCommandEnd(new MovementCommandEnd(1, 1, 20, endState)));

        var oldState = new MovementState(Vector3.Zero, Vector3.Zero, Vector3.Forward, 1, 4);
        var oldAck = new MovementStateAck(1, 4, 19, oldState, 0);
        Assert.False(predictor.ApplyAck(oldAck, out _, out _));
        Assert.Equal(3.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorRejectsStaleMovementCommandEnd()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);
        Assert.True(predictor.ApplyCommandStart(CommandStart()));
        var ackState = new MovementState(new Vector3(0.0f, 0.0f, 3.0f),
                                         Vector3.Zero, Vector3.Forward, 1, 5);
        var ack = new MovementStateAck(1, 5, 30, ackState, 0);
        Assert.True(predictor.ApplyAck(ack, out _, out _));

        var staleState = new MovementState(Vector3.Zero, Vector3.Zero, Vector3.Forward, 1, 4);

        Assert.False(predictor.ApplyCommandEnd(new MovementCommandEnd(1, 1, 20,
                                                                      staleState)));
        Assert.True(predictor.HasActiveCommand);
        Assert.Equal(3.0f, predictor.PredictedPosition.Z, 3);
    }

    [Fact]
    public void OwnerPredictorRejectsInvalidMovementCommandStart()
    {
        var predictor = new OwnerMovementPredictor(FakePredictStep, 8);
        predictor.Reset(Vector3.Zero, Vector3.Forward);

        var invalid = new MovementCommandStart(1,
            new ClientMovementCommand(1, 1, MovementCommandType.Dash, Vector3.Zero,
                                      Vector3.Forward, 0, 0, 0,
                                      MovementCommandInputPolicy.Suppress,
                                      MovementCommandCollisionPolicy.Stop, 0, 0));

        Assert.False(predictor.ApplyCommandStart(invalid));
        Assert.False(predictor.HasActiveCommand);

        Assert.False(predictor.ApplyCommandStart(CommandStart(commandId: 0)));
        Assert.False(predictor.HasActiveCommand);

        Assert.False(predictor.ApplyCommandStart(CommandStart(
            inputPolicy: MovementCommandInputPolicy.AllowFull)));
        Assert.False(predictor.HasActiveCommand);
    }

    static AtlasMovementInputFrame Frame(uint seq, sbyte moveZ = 0) =>
        new()
        {
            Seq = seq,
            InputTick = seq + 100,
            MoveZ = moveZ,
            ClientDtMs = 33,
        };

    static MovementCommandStart CommandStart(ushort elapsedMs = 0, ushort curveId = 0,
                                             uint commandId = 1, uint serverTick = 10,
                                             byte priority = 0,
                                             MovementCommandInputPolicy inputPolicy =
                                                 MovementCommandInputPolicy.Suppress) =>
        new(1, new ClientMovementCommand(commandId, 1, MovementCommandType.Dash, Vector3.Zero,
                                         new Vector3(0.0f, 0.0f, 4.0f), 1000,
                                         elapsedMs, curveId,
                                         inputPolicy,
                                         MovementCommandCollisionPolicy.Stop,
                                         priority, serverTick));

    static bool FakePredictStep(AtlasMovementStateFrame previous,
                                AtlasMovementInputFrame input,
                                uint serverTick,
                                out AtlasMovementStateFrame next)
    {
        _ = serverTick;
        next = previous;
        next.PositionZ += input.MoveZ / 100.0f;
        next.LastProcessedInputSeq = input.Seq;
        return true;
    }
}
