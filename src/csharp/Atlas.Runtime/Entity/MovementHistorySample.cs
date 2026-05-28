using Atlas.DataTypes;

namespace Atlas.Entity;

public readonly struct MovementHistorySample
{
    public MovementHistorySample(uint serverTick, Vector3 position, Vector3 velocity,
                                 Vector3 direction, uint flags, uint lastProcessedInputSeq)
    {
        ServerTick = serverTick;
        Position = position;
        Velocity = velocity;
        Direction = direction;
        Flags = flags;
        LastProcessedInputSeq = lastProcessedInputSeq;
    }

    public uint ServerTick { get; }
    public Vector3 Position { get; }
    public Vector3 Velocity { get; }
    public Vector3 Direction { get; }
    public uint Flags { get; }
    public uint LastProcessedInputSeq { get; }
}
