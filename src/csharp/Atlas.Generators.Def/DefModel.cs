using System.Collections.Generic;

namespace Atlas.Generators.Def;

internal enum ExposedScope
{
    None,
    OwnClient,
    AllClients,
}

// PropertyScope values map one-to-one to BigWorld's EntityDataFlags
// combinations (lib/entitydef/data_description.hpp:30-41). The architectural
// invariant from BigWorld (data_description.ipp:113-131 — "data only lives
// on a base or a cell but not both") is encoded here via the IsBase / IsCell
// predicates below. Generators consult those predicates to decide which
// side's generated class receives the backing field, the serializer, the
// dirty-tracking setter, and the delta-apply code. Emitting a cell-scope
// field on the base class (as Atlas's pre-M2 generator did) is what made
// baseapp baseline corrupt the client's _hp with stale zeros.
internal enum PropertyScope
{
    CellPrivate,       // BW: 0                                   — cell-only, ghost-free
    CellPublic,        // BW: DATA_GHOSTED                        — cell, synced to ghosts
    OwnClient,         // BW: DATA_GHOSTED|DATA_OWN_CLIENT        — cell, owner sees
    OtherClients,      // BW: DATA_GHOSTED|DATA_OTHER_CLIENT      — cell, non-owner peers see
    AllClients,        // BW: DATA_GHOSTED|DATA_OWN|DATA_OTHER    — cell, everyone sees
    CellPublicAndOwn,  // BW: DATA_GHOSTED|DATA_OWN_CLIENT        — cell, owner sees (synonym)
    Base,              // BW: DATA_BASE                           — base-only
    BaseAndClient,     // BW: DATA_BASE|DATA_OWN_CLIENT           — base, owner sees
}

internal static class PropertyScopeExtensions
{
    // BigWorld's DataDescription::isBaseData(): returns true iff DATA_BASE
    // bit is set. Atlas keeps it enum-driven rather than bitwise because
    // the Scope value is a fixed combination parsed from XML, not something
    // scripts compose at runtime.
    public static bool IsBase(this PropertyScope scope) =>
        scope == PropertyScope.Base || scope == PropertyScope.BaseAndClient;

    // BigWorld's DataDescription::isCellData(): return !isBaseData(). Pairs
    // exactly with IsBase — no scope value straddles both sides.
    public static bool IsCell(this PropertyScope scope) => !scope.IsBase();

    // BigWorld's DataDescription::isGhostedData(): DATA_GHOSTED bit. Every
    // client-visible cell property ghosts to peer cells so AoI peers on a
    // different cell can observe it; CellPrivate and base-side scopes do
    // not.
    public static bool IsGhosted(this PropertyScope scope) => scope switch
    {
        PropertyScope.CellPublic => true,
        PropertyScope.OwnClient => true,
        PropertyScope.OtherClients => true,
        PropertyScope.AllClients => true,
        PropertyScope.CellPublicAndOwn => true,
        _ => false,
    };

    // BigWorld's DataDescription::isOwnClientData(): DATA_OWN_CLIENT bit.
    public static bool IsOwnClientVisible(this PropertyScope scope) => scope switch
    {
        PropertyScope.OwnClient => true,
        PropertyScope.AllClients => true,
        PropertyScope.CellPublicAndOwn => true,
        PropertyScope.BaseAndClient => true,
        _ => false,
    };

    // BigWorld's DataDescription::isOtherClientData(): DATA_OTHER_CLIENT bit.
    public static bool IsOtherClientsVisible(this PropertyScope scope) => scope switch
    {
        PropertyScope.OtherClients => true,
        PropertyScope.AllClients => true,
        _ => false,
    };

    // Any client visibility — the union of own and other. BigWorld's
    // DataDescription::isClientServerData() is roughly this, modulo the
    // DATA_REPLAY bit Atlas doesn't yet support.
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
    // property (IsCell covers everything except Base / BaseAndClient). Mirrors
    // BigWorld's "does this entity have a cell part?" check on the EntityType
    // description.
    public bool HasCell =>
        CellMethods.Count > 0 ||
        Properties.Exists(p => p.Scope.IsCell());

    // Client-facing when it has any client-visible property or any client RPC.
    public bool HasClient =>
        ClientMethods.Count > 0 ||
        Properties.Exists(p => p.Scope.IsClientVisible());
}
