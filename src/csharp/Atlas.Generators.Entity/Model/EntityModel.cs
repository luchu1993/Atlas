using System.Collections.Generic;

namespace Atlas.Generators.Entity.Model;

/// <summary>
/// Parsed model of an [Entity]-annotated class, extracted from Roslyn syntax/symbols.
/// </summary>
internal sealed class EntityModel
{
    public string Namespace { get; set; } = "";
    public string ClassName { get; set; } = "";
    public string TypeName { get; set; } = "";
    public bool IsPartial { get; set; }
    public bool InheritsServerEntity { get; set; }
    public byte Compression { get; set; }
    public List<FieldModel> Fields { get; } = new();
}

internal sealed class FieldModel
{
    public string FieldName { get; set; } = "";
    public string PropertyName { get; set; } = "";
    public string TypeFullName { get; set; } = "";
    public string WriterMethod { get; set; } = "";
    public string ReaderMethod { get; set; } = "";
    public bool IsReplicated { get; set; }
    public byte ReplicationScope { get; set; }
    public bool IsPersistent { get; set; }
    public bool IsServerOnly { get; set; }
    public bool IsPrivate { get; set; }
    public bool HasUnderscorePrefix { get; set; }
    public bool IsSupportedType { get; set; } = true;
    public byte DataTypeByte { get; set; }
    public byte DetailLevel { get; set; } = 5;
}
