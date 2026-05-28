using Atlas.DataTypes;
using Atlas.Shared.Protocol;

namespace Atlas.Entity;

public readonly struct MovementCommand
{
    public MovementCommand(uint commandId, ushort skillId, MovementCommandType type,
                           Vector3 startPosition, Vector3 targetPosition, ushort durationMs,
                           ushort curveId = 0, ushort elapsedMs = 0,
                           MovementCommandInputPolicy inputPolicy =
                               MovementCommandInputPolicy.Suppress,
                           MovementCommandCollisionPolicy collisionPolicy =
                               MovementCommandCollisionPolicy.Stop,
                           byte priority = 0, uint serverTick = 0)
    {
        CommandId = commandId;
        SkillId = skillId;
        Type = type;
        StartPosition = startPosition;
        TargetPosition = targetPosition;
        DurationMs = durationMs;
        CurveId = curveId;
        ElapsedMs = elapsedMs;
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
