using System.Collections.Generic;

namespace Atlas.Generators.Def;

internal enum ExposedScope
{
    None,
    OwnClient,
    AllClients,
}

// The architectural invariant "data lives on either base or cell but
// not both" is encoded here via the IsBase / IsCell predicates below.
// Generators consult those predicates to decide which side's generated
// class receives the backing field, the serializer, the dirty-tracking
// setter, and the delta-apply code. Emitting a cell-scope field on the
// base class corrupts the client's view — baseline on the base side
// would overwrite cell-authoritative state with stale zeros.
internal enum PropertyScope
{
    CellPrivate,       // cell-only, not ghosted to peer cells
    CellPublic,        // cell, ghosted to peer cells
    OwnClient,         // cell, owning client sees
    OtherClients,      // cell, non-owner peers see
    AllClients,        // cell, everyone sees
    CellPublicAndOwn,  // cell, owning client sees (synonym of OwnClient)
    Base,              // base-only
    BaseAndClient,     // base, owning client sees
}

internal static class PropertyScopeExtensions
{
    // True for scopes that land on the base side (Base, BaseAndClient).
    public static bool IsBase(this PropertyScope scope) =>
        scope == PropertyScope.Base || scope == PropertyScope.BaseAndClient;

    // Pairs exactly with IsBase — no scope value straddles both sides.
    public static bool IsCell(this PropertyScope scope) => !scope.IsBase();

    // Ghosted = replicated to peer cells so AoI peers on a different cell
    // can observe. CellPrivate and base-side scopes are not ghosted.
    public static bool IsGhosted(this PropertyScope scope) => scope switch
    {
        PropertyScope.CellPublic => true,
        PropertyScope.OwnClient => true,
        PropertyScope.OtherClients => true,
        PropertyScope.AllClients => true,
        PropertyScope.CellPublicAndOwn => true,
        _ => false,
    };

    public static bool IsOwnClientVisible(this PropertyScope scope) => scope switch
    {
        PropertyScope.OwnClient => true,
        PropertyScope.AllClients => true,
        PropertyScope.CellPublicAndOwn => true,
        PropertyScope.BaseAndClient => true,
        _ => false,
    };

    public static bool IsOtherClientsVisible(this PropertyScope scope) => scope switch
    {
        PropertyScope.OtherClients => true,
        PropertyScope.AllClients => true,
        _ => false,
    };

    // Union of own and other client visibility.
    public static bool IsClientVisible(this PropertyScope scope) =>
        scope.IsOwnClientVisible() || scope.IsOtherClientsVisible();
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

    // IsReservedPosition: set by DefParser when the .def declares a property
    // named "position" (case-insensitive) with a replicable scope. Position
    // is already transported by the volatile channel and handled by the
    // ClientEntity base class; this flag tells all emitters to skip the
    // property as if the declaration were absent. See ATLAS_DEF008.
    public bool IsReservedPosition { get; set; }
}

internal sealed class EntityDefModel
{
    public string Name { get; set; } = "";
    public int? ExplicitTypeId { get; set; }
    public List<PropertyDefModel> Properties { get; } = new();
    public List<MethodDefModel> ClientMethods { get; } = new();
    public List<MethodDefModel> CellMethods { get; } = new();
    public List<MethodDefModel> BaseMethods { get; } = new();

    // An entity is cell-resident if it exposes cell methods or any cell-side
    // property (IsCell covers everything except Base / BaseAndClient).
    public bool HasCell =>
        CellMethods.Count > 0 ||
        Properties.Exists(p => p.Scope.IsCell());

    // Client-facing when it has any client-visible property or any client RPC.
    public bool HasClient =>
        ClientMethods.Count > 0 ||
        Properties.Exists(p => p.Scope.IsClientVisible());
}
