using System;

namespace Atlas.Entity;

[AttributeUsage(AttributeTargets.Class)]
public sealed class EntityAttribute : Attribute
{
    public string TypeName { get; }
    public EntityAttribute(string typeName) => TypeName = typeName;

    // 0 = None, 1 = Deflate. Applies to entity create / full sync only.
    public byte Compression { get; set; } = 0;
}
