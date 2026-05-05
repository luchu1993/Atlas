using System;

namespace Atlas.DataTypes;

public readonly struct EntityRef : IEquatable<EntityRef>
{
    public readonly uint EntityId;
    public readonly string TypeName;

    public EntityRef(uint entityId, string typeName)
    {
        EntityId = entityId;
        TypeName = typeName ?? throw new ArgumentNullException(nameof(typeName));
    }

    public bool IsValid => EntityId != 0;

    public static bool operator ==(EntityRef a, EntityRef b) => a.Equals(b);
    public static bool operator !=(EntityRef a, EntityRef b) => !a.Equals(b);

    public bool Equals(EntityRef other)
        => EntityId == other.EntityId && TypeName == other.TypeName;

    public override bool Equals(object? obj)
        => obj is EntityRef other && Equals(other);

    public override int GetHashCode()
        => HashCode.Combine(EntityId, TypeName);

    public override string ToString()
        => $"EntityRef({TypeName}#{EntityId})";
}
