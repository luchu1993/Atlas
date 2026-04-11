namespace Atlas.Entity;

/// <summary>
/// Controls what volatile (position/direction) data is sent for an entity
/// and at what priority. Inspired by BigWorld's VolatileInfo.
/// </summary>
public sealed class VolatileInfo
{
    /// <summary>Send priority for position updates. 0 = never, >0 = priority (lower = higher).</summary>
    public float PositionPriority { get; set; } = -1f;

    /// <summary>Send priority for yaw. 0 = never.</summary>
    public float YawPriority { get; set; } = -1f;

    /// <summary>Send priority for pitch. 0 = never.</summary>
    public float PitchPriority { get; set; } = -1f;

    /// <summary>Send priority for roll. 0 = never (rarely needed).</summary>
    public float RollPriority { get; set; } = 0f;

    public bool ShouldSendPosition => PositionPriority > 0f;
    public bool ShouldSendYaw => YawPriority > 0f;
    public bool ShouldSendPitch => PitchPriority > 0f;

    /// <summary>Always send position and yaw at default priority.</summary>
    public static readonly VolatileInfo Default = new()
    {
        PositionPriority = 1f,
        YawPriority = 1f,
        PitchPriority = 0f
    };

    /// <summary>Never send volatile data (static entity).</summary>
    public static readonly VolatileInfo Never = new();
}
