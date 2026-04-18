using System.Collections.Generic;

namespace Atlas.Generators.Def;

internal enum ExposedScope
{
    None,
    OwnClient,
    AllClients,
}

internal enum PropertyScope
{
    CellPrivate,
    CellPublic,
    OwnClient,
    OtherClients,
    AllClients,
    CellPublicAndOwn,
    Base,
    BaseAndClient,
}

internal sealed class ArgDefModel
{
    public string Name { get; set; } = "";
    public string Type { get; set; } = "";
}

internal sealed class MethodDefModel
{
    public string Name { get; set; } = "";
    public ExposedScope Exposed { get; set; } = ExposedScope.None;
    public List<ArgDefModel> Args { get; } = new();
}

internal sealed class PropertyDefModel
{
    public string Name { get; set; } = "";
    public string Type { get; set; } = "";
    public PropertyScope Scope { get; set; } = PropertyScope.CellPrivate;
    public bool Persistent { get; set; }

    // Reliable delivery: when true, changes are routed through the reliable
    // message channel, bypassing the DeltaForwarder byte budget so a dropped
    // packet cannot strand the client in a stale state.
    public bool Reliable { get; set; }
}

internal sealed class EntityDefModel
{
    public string Name { get; set; } = "";
    public int? ExplicitTypeId { get; set; }
    public List<PropertyDefModel> Properties { get; } = new();
    public List<MethodDefModel> ClientMethods { get; } = new();
    public List<MethodDefModel> CellMethods { get; } = new();
    public List<MethodDefModel> BaseMethods { get; } = new();

    public bool HasCell =>
        CellMethods.Count > 0 ||
        Properties.Exists(p => p.Scope == PropertyScope.CellPrivate ||
                               p.Scope == PropertyScope.CellPublic ||
                               p.Scope == PropertyScope.CellPublicAndOwn);

    public bool HasClient =>
        ClientMethods.Count > 0 ||
        Properties.Exists(p => p.Scope == PropertyScope.OwnClient ||
                               p.Scope == PropertyScope.AllClients ||
                               p.Scope == PropertyScope.OtherClients ||
                               p.Scope == PropertyScope.BaseAndClient);
}
