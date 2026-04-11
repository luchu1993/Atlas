namespace Atlas.Runtime.Entity;

/// <summary>
/// Tracks per-client replication state for a single entity.
/// Used by Witness to know what each client has received.
/// </summary>
public sealed class ClientReplicationState
{
    /// <summary>Last delta sequence this client has acknowledged.</summary>
    public uint LastDeltaSeq { get; set; }

    /// <summary>ID alias for this entity on this client (0 = not assigned).</summary>
    public byte IdAlias { get; set; }

    /// <summary>Whether this entity has been fully created on this client.</summary>
    public bool IsCreated { get; set; }

    /// <summary>Current LOD detail level for this client (based on distance).</summary>
    public byte CurrentDetailLevel { get; set; } = 5;
}
