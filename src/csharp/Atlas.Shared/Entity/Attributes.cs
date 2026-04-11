using System;

namespace Atlas.Entity;

[AttributeUsage(AttributeTargets.Class)]
public sealed class EntityAttribute : Attribute
{
    public string TypeName { get; }
    public EntityAttribute(string typeName) => TypeName = typeName;

    /// <summary>
    /// Compression type for large messages (entity creation, full sync) sent to clients.
    /// 0 = None, 1 = Deflate. Default: None.
    /// </summary>
    public byte Compression { get; set; } = 0;
}

[AttributeUsage(AttributeTargets.Field)]
public sealed class ReplicatedAttribute : Attribute
{
    public ReplicationScope Scope { get; set; } = ReplicationScope.AllClients;

    /// <summary>
    /// Level of detail tier (0-5). Lower values require closer proximity to be sent.
    /// 0 = sent only to nearest clients, 5 = sent to all clients in AoI.
    /// Default is 5 (always sent to all AoI clients).
    /// </summary>
    public byte DetailLevel { get; set; } = 5;
}

public enum ReplicationScope : byte
{
    CellPrivate = 0,
    BaseOnly    = 1,
    OwnClient   = 2,
    AllClients  = 3,
}

[AttributeUsage(AttributeTargets.Field)]
public sealed class PersistentAttribute : Attribute { }

[AttributeUsage(AttributeTargets.Field)]
public sealed class ServerOnlyAttribute : Attribute { }
