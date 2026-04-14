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
