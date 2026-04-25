using System.Collections.Generic;

namespace Atlas.Generators.Def;

// Mirrors atlas::PropertyDataType (entity_type_descriptor.h). Values must
// stay in lockstep with the C++ enum — they are the wire encoding.
internal enum PropertyDataKind : byte
{
    Bool = 0,
    Int8 = 1,
    UInt8 = 2,
    Int16 = 3,
    UInt16 = 4,
    Int32 = 5,
    UInt32 = 6,
    Int64 = 7,
    UInt64 = 8,
    Float = 9,
    Double = 10,
    String = 11,
    Bytes = 12,
    Vector3 = 13,
    Quaternion = 14,
    Custom = 15,
    List = 16,
    Dict = 17,
    Struct = 18,
}

// Recursive type descriptor — C# mirror of atlas::DataTypeRef. Scalar kinds
// leave Elem / Key / StructName / StructId unused; container kinds populate
// the matching slots. StructId stays -1 until DefLinker resolves struct
// names into stable 16-bit ids.
//
// Treat as immutable once DefLinker.Link has returned: alias expansion
// shares subtree references across multiple TypeRefs, so a post-link
// mutation to one tree can leak into unrelated properties. Emitters are
// read-only; if a future pass (e.g. kStructFieldSet metadata caching)
// needs to attach data, it must do so on a side-table keyed by
// DataTypeRefModel identity rather than by mutating the node.
internal sealed class DataTypeRefModel
{
    public PropertyDataKind Kind { get; set; }
    public string? StructName { get; set; }
    public int StructId { get; set; } = -1;
    public DataTypeRefModel? Elem { get; set; }
    public DataTypeRefModel? Key { get; set; }

    public bool IsContainer =>
        Kind == PropertyDataKind.List ||
        Kind == PropertyDataKind.Dict ||
        Kind == PropertyDataKind.Struct;
}

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
    // Set for container / struct args. Mirrors PropertyDefModel.TypeRef
    // so emitters can route container / struct args through the same
    // PropertyCodec helpers as properties. Null = scalar (then `Type`
    // alone pins the C# type).
    public DataTypeRefModel? TypeRef { get; set; }
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

    // Recursive type descriptor. Populated by DefParser when Type is a
    // container (list/dict) or a struct/alias reference. Null for scalars;
    // the legacy `Type` string keeps driving scalar emitters.
    public DataTypeRefModel? TypeRef { get; set; }

    // Container-only element cap. Scalar properties ignore this field.
    public uint MaxSize { get; set; } = 4096;
}

internal sealed class FieldDefModel
{
    public string Name { get; set; } = "";
    public DataTypeRefModel Type { get; set; } = new();
}

// Controls how a struct's mutations are wired:
//
//   Whole  — atomic value; setters replace the struct and serialise
//            all fields together. Zero-GC, good for small structs.
//   Field  — each field gets its own wire op (kStructFieldSet). Saves
//            bandwidth on wide structs whose hot field is a small slice
//            of the body. Not yet implemented.
//   Auto   — emitter picks between Whole and Field via the heuristic
//            in CONTAINER_PROPERTY_SYNC_DESIGN §9 and surfaces its
//            choice via an Info diagnostic.
internal enum StructSyncMode : byte
{
    Auto,
    Whole,
    Field,
}

internal sealed class StructDefModel
{
    public string Name { get; set; } = "";

    // Stable 16-bit handle used on the wire. Assigned by DefLinker once all
    // defs have been parsed so struct references can be resolved. -1 means
    // unassigned (parser-only state).
    public int Id { get; set; } = -1;

    // User-declared sync= attribute, defaults to Auto. The emitter may
    // coerce Auto to Whole or Field based on struct shape.
    public StructSyncMode SyncMode { get; set; } = StructSyncMode.Auto;

    public List<FieldDefModel> Fields { get; } = new();
}

internal sealed class AliasDefModel
{
    public string Name { get; set; } = "";
    public DataTypeRefModel Target { get; set; } = new();
}

// One Component slot declaration on the entity. SlotIdx is assigned by
// DefLinker (1-based; slot 0 is reserved for the entity body). For
// inline components, Properties is populated; for cross-file references
// (future), Properties stays empty until linker resolution runs.
internal sealed class ComponentDefModel
{
    public string SlotName { get; set; } = "";    // e.g., "combat" — script-facing identifier
    public string TypeName { get; set; } = "";    // e.g., "CombatComponent" — generated class name
    public PropertyScope Scope { get; set; } = PropertyScope.AllClients;
    public bool Lazy { get; set; }
    public ComponentLocality Locality { get; set; } = ComponentLocality.Synced;
    public int SlotIdx { get; set; } = -1;
    public List<PropertyDefModel> Properties { get; } = new();

    // RPC sections — same shape as EntityDefModel. Component RPCs are
    // routed via the entity (which carries a slot_idx in rpc_id bits
    // 24-31), so they belong on the component's def even though dispatch
    // happens on the entity's typeIndex.
    public List<MethodDefModel> ClientMethods { get; } = new();
    public List<MethodDefModel> CellMethods { get; } = new();
    public List<MethodDefModel> BaseMethods { get; } = new();

    // Component inheritance: derived component classes extend a base
    // component with additional properties. propIdx is hierarchy-flat
    // — derived's first property starts at PropIdxBase. Set by the
    // linker after resolving the chain.
    public string? BaseTypeName { get; set; }
    public ComponentDefModel? Base { get; set; }
    public int PropIdxBase { get; set; }

    // Standalone components are declared in their own .def file (root
    // is `<component>`) and referenced by name from entity slots.
    // Inline components live within an `<entity><components>` block
    // and aren't in the cross-entity registry.
    public bool IsStandalone { get; set; }

    // Set once the linker has merged the entity's slot info (for
    // inline) or the standalone definition resolves it. Used by
    // ComponentEmitter to skip already-emitted duplicates.
    public bool IsEmitted { get; set; }
}

internal enum ComponentLocality
{
    Synced,
    ServerLocal,
    ClientLocal,
}

internal sealed class EntityDefModel
{
    public string Name { get; set; } = "";
    public int? ExplicitTypeId { get; set; }
    public List<PropertyDefModel> Properties { get; } = new();
    public List<MethodDefModel> ClientMethods { get; } = new();
    public List<MethodDefModel> CellMethods { get; } = new();
    public List<MethodDefModel> BaseMethods { get; } = new();

    // Entity-local <types> declarations. Aliases resolve only within this
    // entity's scope; structs do the same for now. DefLinker will later be
    // extended to support a shared types.xml when the generator grows a
    // cross-file pass.
    public List<StructDefModel> Structs { get; } = new();
    public List<AliasDefModel> Aliases { get; } = new();

    // Synced + local components declared in <components>. Slot indices
    // for Synced ones are assigned by DefLinker; local components have
    // SlotIdx = -1 (they don't participate in the slot table).
    public List<ComponentDefModel> Components { get; } = new();

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
