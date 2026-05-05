namespace Atlas.Entity;

public sealed class VolatileInfo
{
    // Priority: 0 = never, > 0 = priority (lower = higher).
    public float PositionPriority { get; set; } = -1f;
    public float YawPriority { get; set; } = -1f;
    public float PitchPriority { get; set; } = -1f;
    public float RollPriority { get; set; } = 0f;

    public bool ShouldSendPosition => PositionPriority > 0f;
    public bool ShouldSendYaw => YawPriority > 0f;
    public bool ShouldSendPitch => PitchPriority > 0f;

    public static readonly VolatileInfo Default = new()
    {
        PositionPriority = 1f,
        YawPriority = 1f,
        PitchPriority = 0f,
    };

    public static readonly VolatileInfo Never = new();
}
