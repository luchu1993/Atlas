using Atlas.DataTypes;
using Atlas.Shared.Protocol;

namespace Atlas.Client;

public enum MovementCorrectionTier : ushort
{
    None = 0,
    Tier1 = 1,
    Tier2 = 2,
    Snap = 3,
}

public static class MovementCorrection
{
    public const float Tier1DistanceM = 0.3f;
    public const float Tier2DistanceM = 1.5f;
    public const float SnapDistanceM = 5.0f;
    public const ushort Tier1Flag = 1 << 0;
    public const ushort Tier2Flag = 1 << 1;
    public const ushort SnapFlag = 1 << 2;

    public static MovementCorrectionTier Classify(float distanceM)
    {
        if (float.IsNaN(distanceM) || float.IsInfinity(distanceM) || distanceM < Tier1DistanceM)
            return MovementCorrectionTier.None;
        if (distanceM >= SnapDistanceM) return MovementCorrectionTier.Snap;
        if (distanceM >= Tier2DistanceM) return MovementCorrectionTier.Tier2;
        return MovementCorrectionTier.Tier1;
    }

    public static ushort FlagFor(MovementCorrectionTier tier) =>
        tier switch
        {
            MovementCorrectionTier.Tier1 => Tier1Flag,
            MovementCorrectionTier.Tier2 => Tier2Flag,
            MovementCorrectionTier.Snap => SnapFlag,
            _ => 0,
        };

    public static bool IsFlagsValid(ushort flags) =>
        flags == 0 || flags == Tier1Flag || flags == Tier2Flag || flags == SnapFlag;
}

public readonly struct MovementState
{
    public MovementState(Vector3 position, Vector3 velocity, Vector3 direction, uint flags,
                         uint lastProcessedInputSeq)
    {
        Position = position;
        Velocity = velocity;
        Direction = direction;
        Flags = flags;
        LastProcessedInputSeq = lastProcessedInputSeq;
    }

    public Vector3 Position { get; }
    public Vector3 Velocity { get; }
    public Vector3 Direction { get; }
    public uint Flags { get; }
    public uint LastProcessedInputSeq { get; }
}

public readonly struct MovementStateAck
{
    public MovementStateAck(uint entityId, uint ackedInputSeq, uint serverTick,
                            MovementState state, ushort correctionFlags)
    {
        EntityId = entityId;
        AckedInputSeq = ackedInputSeq;
        ServerTick = serverTick;
        State = state;
        CorrectionFlags = correctionFlags;
    }

    public uint EntityId { get; }
    public uint AckedInputSeq { get; }
    public uint ServerTick { get; }
    public MovementState State { get; }
    public ushort CorrectionFlags { get; }
}

public readonly struct ClientMovementCommand
{
    public ClientMovementCommand(uint commandId, ushort skillId, MovementCommandType type,
                                 Vector3 startPosition, Vector3 targetPosition,
                                 ushort durationMs, ushort elapsedMs, ushort curveId,
                                 MovementCommandInputPolicy inputPolicy,
                                 MovementCommandCollisionPolicy collisionPolicy,
                                 byte priority, uint serverTick)
    {
        CommandId = commandId;
        SkillId = skillId;
        Type = type;
        StartPosition = startPosition;
        TargetPosition = targetPosition;
        DurationMs = durationMs;
        ElapsedMs = elapsedMs;
        CurveId = curveId;
        InputPolicy = inputPolicy;
        CollisionPolicy = collisionPolicy;
        Priority = priority;
        ServerTick = serverTick;
    }

    public uint CommandId { get; }
    public ushort SkillId { get; }
    public MovementCommandType Type { get; }
    public Vector3 StartPosition { get; }
    public Vector3 TargetPosition { get; }
    public ushort DurationMs { get; }
    public ushort ElapsedMs { get; }
    public ushort CurveId { get; }
    public MovementCommandInputPolicy InputPolicy { get; }
    public MovementCommandCollisionPolicy CollisionPolicy { get; }
    public byte Priority { get; }
    public uint ServerTick { get; }
}

public readonly struct MovementCommandStart
{
    public MovementCommandStart(uint entityId, ClientMovementCommand command)
    {
        EntityId = entityId;
        Command = command;
    }

    public uint EntityId { get; }
    public ClientMovementCommand Command { get; }
}

public readonly struct MovementCommandEnd
{
    public MovementCommandEnd(uint entityId, uint commandId, uint serverTick,
                              MovementState state,
                              MovementCommandEndReason reason = MovementCommandEndReason.Completed)
    {
        EntityId = entityId;
        CommandId = commandId;
        ServerTick = serverTick;
        Reason = reason;
        State = state;
    }

    public uint EntityId { get; }
    public uint CommandId { get; }
    public uint ServerTick { get; }
    public MovementCommandEndReason Reason { get; }
    public MovementState State { get; }
}
