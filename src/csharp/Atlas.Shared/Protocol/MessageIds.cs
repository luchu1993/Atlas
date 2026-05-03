namespace Atlas.Protocol;

/// <summary>
/// Well-known message ID constants for the Atlas wire protocol.
/// IDs 1–127 are packed as a single byte on the wire for efficiency.
/// </summary>
public static class MessageIds
{
    public const int EntityCreate       = 1;
    public const int EntityDestroy      = 2;
    public const int EntityPropertySync = 3;
    public const int EntityDeltaSync    = 4;
    public const int RpcMessage         = 5;
    public const int EventMessage       = 6;

    // Volatile position updates (unreliable, high frequency)
    public const int VolatilePosition       = 7;   // position only (12 bytes)
    public const int VolatilePositionYaw    = 8;   // position + yaw (14 bytes)
    public const int VolatilePositionYawPitch = 9; // position + yaw + pitch (15 bytes)

    // Aliased variants: use 1-byte entity ID alias instead of 4-byte EntityID
    public const int EntityDeltaSyncAliased    = 10;
    public const int RpcMessageAliased         = 11;
    public const int VolatilePositionAliased   = 12;

    // Server-side reply for await-style entity RPCs. Mirrors C++
    // atlas::msg_id::Common::kEntityRpcReply.
    public const int EntityRpcReply            = 102;
}
