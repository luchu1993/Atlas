using System;
using Atlas.Client.Native;
using Atlas.DataTypes;

namespace Atlas.Client;

public sealed class OwnerMovementPredictor
{
    public delegate bool PredictStepDelegate(AtlasMovementStateFrame previous,
                                             AtlasMovementInputFrame input,
                                             uint serverTick,
                                             out AtlasMovementStateFrame next);

    const int kDefaultHistoryCapacity = 96;
    const float kSlowDecayMps = 3.0f;
    const float kFastDecayMps = 12.0f;
    const uint kGroundedFlag = 1u;

    readonly MovementInputHistory _history;
    readonly PredictStepDelegate _predictStep;
    readonly MovementCommandPlayback _activeCommand;
    uint _lastAckSeq;
    uint _lastAckServerTick;
    uint _lastCommandStartServerTick;
    uint _lastCommandStartId;
    byte _lastCommandStartPriority;
    bool _hasLastAckSeq;
    bool _hasLastCommandStart;
    bool _hasOpenCommandStart;
    bool _hasCommandTurnInput;
    AtlasMovementStateFrame _state;
    Vector3 _visualOffset;
    float _visualOffsetDecayMps;

    public OwnerMovementPredictor(PredictStepDelegate predictStep,
                                  int historyCapacity = kDefaultHistoryCapacity,
                                  IMovementCurveSampler? curves = null)
    {
        _predictStep = predictStep ?? throw new ArgumentNullException(nameof(predictStep));
        _history = new MovementInputHistory(historyCapacity);
        _activeCommand = new MovementCommandPlayback(curves);
    }

    public int PendingInputCount => _history.Count;
    public Vector3 PredictedPosition => Position(_state);
    public Vector3 RenderPosition => Position(_state) + _visualOffset;
    public Vector3 RenderDirection => Direction(_state);
    public Vector3 VisualOffset => _visualOffset;
    public bool HasActiveCommand => _activeCommand.IsActive;
    public bool AcceptsInput => !_hasOpenCommandStart || _activeCommand.AllowsTurnInput;

    public void Reset(Vector3 position, Vector3 direction)
    {
        _history.Clear();
        _lastAckSeq = 0;
        _lastAckServerTick = 0;
        _lastCommandStartServerTick = 0;
        _lastCommandStartId = 0;
        _lastCommandStartPriority = 0;
        _hasLastAckSeq = false;
        _hasLastCommandStart = false;
        _hasOpenCommandStart = false;
        _hasCommandTurnInput = false;
        _activeCommand.Clear();
        _visualOffset = Vector3.Zero;
        _visualOffsetDecayMps = 0.0f;
        _state = new AtlasMovementStateFrame
        {
            PositionX = position.X,
            PositionY = position.Y,
            PositionZ = position.Z,
            DirectionX = direction.X,
            DirectionY = direction.Y,
            DirectionZ = direction.Z,
            Flags = kGroundedFlag,
        };
    }

    public bool PushInput(AtlasMovementInputFrame input)
    {
        if (!ApplyInput(input, input.InputTick)) return false;
        _history.Push(input);
        return true;
    }

    public bool ApplyCommandStart(MovementCommandStart start)
    {
        if (IsCommandStartStale(start.Command)) return false;
        if (!_activeCommand.Start(start.Command)) return false;

        _lastCommandStartServerTick = start.Command.ServerTick;
        _lastCommandStartId = start.Command.CommandId;
        _lastCommandStartPriority = start.Command.Priority;
        _hasLastCommandStart = true;
        _hasOpenCommandStart = true;
        _hasCommandTurnInput = false;
        Vector3 renderBefore = RenderPosition;
        _history.Clear();
        ApplyCommandSample(0);
        ApplyVisualCorrection(renderBefore);
        return true;
    }

    public bool ApplyCommandEnd(MovementCommandEnd end)
    {
        if (end.CommandId == 0) return false;
        if (IsBehindLastCommandStart(end.ServerTick)) return false;
        if (MovementSequence.IsAckStale(end.State.LastProcessedInputSeq, end.ServerTick,
                                        _lastAckSeq, _lastAckServerTick, _hasLastAckSeq))
            return false;

        if (_activeCommand.CommandId != 0 && _activeCommand.CommandId != end.CommandId)
            return false;

        Vector3 renderBefore = RenderPosition;
        _activeCommand.Clear();
        _hasOpenCommandStart = false;
        _hasCommandTurnInput = false;
        ApplyAuthoritativeCheckpoint(end.State, end.State.LastProcessedInputSeq,
                                     end.ServerTick, alignActiveCommand: false);
        ApplyVisualCorrection(renderBefore);
        return true;
    }

    bool IsCommandStartStale(ClientMovementCommand command)
    {
        if (_hasLastAckSeq && command.ServerTick < _lastAckServerTick) return true;
        if (!_hasLastCommandStart) return false;
        if (command.ServerTick < _lastCommandStartServerTick) return true;
        if (command.ServerTick != _lastCommandStartServerTick) return false;
        if (command.CommandId == _lastCommandStartId) return true;
        return _hasOpenCommandStart && command.Priority <= _lastCommandStartPriority;
    }

    bool IsBehindLastCommandStart(uint serverTick) =>
        _hasLastCommandStart && serverTick < _lastCommandStartServerTick;

    public bool ApplyAck(MovementStateAck ack, out float correctionDistanceM,
                         out ushort correctionFlags)
    {
        correctionDistanceM = 0.0f;
        correctionFlags = 0;
        if (IsBehindLastCommandStart(ack.ServerTick)) return false;
        if (MovementSequence.IsAckStale(ack.AckedInputSeq, ack.ServerTick, _lastAckSeq,
                                        _lastAckServerTick, _hasLastAckSeq))
            return false;

        Vector3 renderBefore = RenderPosition;
        ApplyAuthoritativeCheckpoint(ack.State, ack.AckedInputSeq, ack.ServerTick,
                                     alignActiveCommand: true);

        Vector3 corrected = Position(_state);
        correctionDistanceM = Vector3.Distance(renderBefore, corrected);
        var tier = MovementCorrection.Classify(correctionDistanceM);
        correctionFlags = MovementCorrection.FlagFor(tier);
        if (tier == MovementCorrectionTier.Snap)
        {
            _visualOffset = Vector3.Zero;
            _visualOffsetDecayMps = 0.0f;
        }
        else if (tier != MovementCorrectionTier.None)
        {
            _visualOffset = renderBefore - corrected;
            _visualOffsetDecayMps =
                tier == MovementCorrectionTier.Tier2 ? kFastDecayMps : kSlowDecayMps;
        }
        return true;
    }

    void ApplyAuthoritativeCheckpoint(MovementState state, uint ackedInputSeq, uint serverTick,
                                      bool alignActiveCommand)
    {
        _lastAckSeq = ackedInputSeq;
        _lastAckServerTick = serverTick;
        _hasLastAckSeq = true;
        _state = ToNativeState(state);
        if (alignActiveCommand) AlignActiveCommandToState(state.Position);
        _history.DropAcked(ackedInputSeq);
        ReplayPending();
    }

    public void TickVisualOffset(float deltaTime)
    {
        AdvanceActiveCommand(deltaTime);
        float distance = _visualOffset.Length;
        if (distance <= 0.0001f)
        {
            _visualOffset = Vector3.Zero;
            return;
        }

        float step = MathF.Max(_visualOffsetDecayMps, kSlowDecayMps)
                     * MathF.Max(deltaTime, 0.0f);
        if (step >= distance)
        {
            _visualOffset = Vector3.Zero;
            return;
        }
        _visualOffset = _visualOffset * ((distance - step) / distance);
    }

    public int CopyRecentFrames(Span<AtlasMovementInputFrame> destination) =>
        _history.CopyRecent(destination);

    void ReplayPending()
    {
        for (int i = 0; i < _history.Count; ++i)
        {
            var input = _history[i];
            if (!ApplyInput(input, input.InputTick)) break;
        }
    }

    bool ApplyInput(AtlasMovementInputFrame input, uint serverTick)
    {
        if (!_hasOpenCommandStart) return TryStep(input, serverTick);
        if (!_activeCommand.AllowsTurnInput) return false;
        ApplyCommandTurnInput(input);
        return true;
    }

    bool TryStep(AtlasMovementInputFrame input, uint serverTick)
    {
        if (!_predictStep(_state, input, serverTick, out var next)) return false;
        _state = next;
        return true;
    }

    void AdvanceActiveCommand(float deltaTime)
    {
        if (!_activeCommand.IsActive) return;
        int deltaMs = Math.Clamp((int)MathF.Round(MathF.Max(deltaTime, 0.0f) * 1000.0f),
                                 0, ushort.MaxValue);
        ApplyCommandSample((uint)deltaMs);
    }

    void ApplyCommandSample(uint deltaMs)
    {
        Vector3 previous = Position(_state);
        _activeCommand.AdvanceMs(deltaMs);
        Vector3 next = _activeCommand.Position;
        _state.PositionX = next.X;
        _state.PositionY = next.Y;
        _state.PositionZ = next.Z;
        if (deltaMs > 0)
        {
            float invDt = 1000.0f / deltaMs;
            var velocity = (next - previous) * invDt;
            _state.VelocityX = velocity.X;
            _state.VelocityY = velocity.Y;
            _state.VelocityZ = velocity.Z;
        }
        else
        {
            _state.VelocityX = 0.0f;
            _state.VelocityY = 0.0f;
            _state.VelocityZ = 0.0f;
        }

        if (_activeCommand.TryGetDirection(out var direction))
        {
            if (!_activeCommand.AllowsTurnInput || !_hasCommandTurnInput)
            {
                _state.DirectionX = direction.X;
                _state.DirectionY = direction.Y;
                _state.DirectionZ = direction.Z;
            }
        }
    }

    void ApplyCommandTurnInput(AtlasMovementInputFrame input)
    {
        Vector3 direction = DirectionFromViewYaw(input.ViewYaw);
        _state.DirectionX = direction.X;
        _state.DirectionY = direction.Y;
        _state.DirectionZ = direction.Z;
        _state.LastProcessedInputSeq = input.Seq;
        _hasCommandTurnInput = true;
    }

    void AlignActiveCommandToState(Vector3 position)
    {
        _activeCommand.AlignToPosition(position);
    }

    void ApplyVisualCorrection(Vector3 renderBefore)
    {
        Vector3 corrected = Position(_state);
        float distance = Vector3.Distance(renderBefore, corrected);
        var tier = MovementCorrection.Classify(distance);
        if (tier == MovementCorrectionTier.Snap)
        {
            _visualOffset = Vector3.Zero;
            _visualOffsetDecayMps = 0.0f;
        }
        else if (tier != MovementCorrectionTier.None)
        {
            _visualOffset = renderBefore - corrected;
            _visualOffsetDecayMps =
                tier == MovementCorrectionTier.Tier2 ? kFastDecayMps : kSlowDecayMps;
        }
    }

    static AtlasMovementStateFrame ToNativeState(MovementState state) =>
        new()
        {
            PositionX = state.Position.X,
            PositionY = state.Position.Y,
            PositionZ = state.Position.Z,
            VelocityX = state.Velocity.X,
            VelocityY = state.Velocity.Y,
            VelocityZ = state.Velocity.Z,
            DirectionX = state.Direction.X,
            DirectionY = state.Direction.Y,
            DirectionZ = state.Direction.Z,
            Flags = state.Flags,
            LastProcessedInputSeq = state.LastProcessedInputSeq,
        };

    static Vector3 Position(AtlasMovementStateFrame state) =>
        new(state.PositionX, state.PositionY, state.PositionZ);

    static Vector3 Direction(AtlasMovementStateFrame state)
    {
        var dir = new Vector3(state.DirectionX, 0.0f, state.DirectionZ);
        return dir.LengthSquared > 0.0001f ? dir.Normalized : Vector3.Forward;
    }

    static Vector3 DirectionFromViewYaw(ushort viewYaw)
    {
        float yaw = (viewYaw / 65535.0f) * MathF.PI * 2.0f;
        return new Vector3(MathF.Sin(yaw), 0.0f, MathF.Cos(yaw));
    }
}
