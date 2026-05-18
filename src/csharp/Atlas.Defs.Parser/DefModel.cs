using System.Collections.Generic;

namespace Atlas.Generators.Def;

// Wire encoding — keep in lockstep with atlas::PropertyDataType.
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

// Immutable post-DefLinker: alias expansion shares subtree refs across
// TypeRefs, so post-link mutation leaks across properties.
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

// IsBase / IsCell predicates pick the side that owns each property; emitting
// a cell-scope field on the base side overwrites authoritative state.
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

    // Replicated to peer cells so off-cell AoI peers can observe.
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
    // Null for scalars (Type alone pins the C# type); set for container/struct.
    public DataTypeRefModel? TypeRef { get; set; }
}

internal sealed class MethodDefModel
{
    public string Name { get; set; } = "";
    public ExposedScope Exposed { get; set; } = ExposedScope.None;
    public List<ArgDefModel> Args { get; } = new();

    // reply="..." attribute. Null = fire-and-forget; non-null = await-reply.
    public string? Reply { get; set; }
    public DataTypeRefModel? ReplyTypeRef { get; set; }
    public bool HasReply => Reply is not null;
}

internal sealed class PropertyDefModel
{
    public string Name { get; set; } = "";
    public string Type { get; set; } = "";
    public PropertyScope Scope { get; set; } = PropertyScope.CellPrivate;
    public bool Persistent { get; set; }

    // Bypasses DeltaForwarder byte budget — dropped packet won't strand the client.
    public bool Reliable { get; set; }

    // ATLAS_DEF008: `position` is volatile-channel transported; emitters skip.
    public bool IsReservedPosition { get; set; }

    // Null for scalars; set for container / struct / alias references.
    public DataTypeRefModel? TypeRef { get; set; }

    // Container-only; scalars ignore.
    public uint MaxSize { get; set; } = 4096;
}

internal sealed class FieldDefModel
{
    public string Name { get; set; } = "";
    public DataTypeRefModel Type { get; set; } = new();
}

// Whole = atomic; Field = per-field op; Auto = StructEmitter picks + DEF014.
internal enum StructSyncMode : byte
{
    Auto,
    Whole,
    Field,
}

internal sealed class StructDefModel
{
    public string Name { get; set; } = "";

    // Wire-stable id assigned by DefLinker; -1 in parser-only state.
    public int Id { get; set; } = -1;

    // User attribute; emitter may coerce Auto → Whole/Field by struct shape.
    public StructSyncMode SyncMode { get; set; } = StructSyncMode.Auto;

    public List<FieldDefModel> Fields { get; } = new();
}

internal sealed class AliasDefModel
{
    public string Name { get; set; } = "";
    public DataTypeRefModel Target { get; set; } = new();
}

// SlotIdx assigned by DefLinker; slot 0 reserved for entity body.
internal sealed class ComponentDefModel
{
    public string SlotName { get; set; } = "";    // e.g., "combat" — script-facing identifier
    public string TypeName { get; set; } = "";    // e.g., "CombatComponent" — generated class name
    public PropertyScope Scope { get; set; } = PropertyScope.AllClients;
    public bool Lazy { get; set; }
    public ComponentLocality Locality { get; set; } = ComponentLocality.Synced;
    public int SlotIdx { get; set; } = -1;

    // Wire-stable handle from component_ids.xml; -1 until DefLinker resolves
    // the manifest. Inline-only components stay -1 (no cross-file lookup).
    public int ComponentTypeId { get; set; } = -1;

    public List<PropertyDefModel> Properties { get; } = new();

    // Component RPCs route via entity (slot_idx in rpc_id bits 24-31).
    public List<MethodDefModel> ClientMethods { get; } = new();
    public List<MethodDefModel> CellMethods { get; } = new();
    public List<MethodDefModel> BaseMethods { get; } = new();

    // Hierarchy-flat propIdx; derived starts at PropIdxBase (set by linker).
    public string? BaseTypeName { get; set; }
    public ComponentDefModel? Base { get; set; }
    public int PropIdxBase { get; set; }

    // Standalone = own .def with <component> root; inline lives inside <entity>.
    public bool IsStandalone { get; set; }

    // Dedup guard for ComponentEmitter when same type appears in multiple entities.
    public bool IsEmitted { get; set; }
}

internal enum ComponentLocality
{
    Synced,
    ServerLocal,
    ClientLocal,
}

// KeyId is a permanent contract — never reused after a key is dropped.
internal sealed class SpaceDataKeyDefModel
{
    public string Name { get; set; } = "";
    public ushort KeyId { get; set; }
    public DataTypeRefModel Type { get; set; } = new();
    public string TypeText { get; set; } = "";
    public bool Deprecated { get; set; }
}

internal sealed class SpaceDataDefModel
{
    public string SourcePath { get; set; } = "";
    public List<SpaceDataKeyDefModel> Keys { get; } = new();
}

internal sealed class EntityDefModel
{
    public string Name { get; set; } = "";
    public List<PropertyDefModel> Properties { get; } = new();
    public List<MethodDefModel> ClientMethods { get; } = new();
    public List<MethodDefModel> CellMethods { get; } = new();
    public List<MethodDefModel> BaseMethods { get; } = new();

    // Entity-local scope; cross-file types.xml is a future pass.
    public List<StructDefModel> Structs { get; } = new();
    public List<AliasDefModel> Aliases { get; } = new();

    public List<ComponentDefModel> Components { get; } = new();

    // null = derive from surface; explicit `has_base` attribute overrides.
    public bool? HasBaseExplicit { get; set; }

    public bool HasBase =>
        HasBaseExplicit ?? (BaseMethods.Count > 0 ||
                            Properties.Exists(p => p.Scope.IsBase()));

    public bool HasCell =>
        CellMethods.Count > 0 ||
        Properties.Exists(p => p.Scope.IsCell());

    public bool HasClient =>
        ClientMethods.Count > 0 ||
        Properties.Exists(p => p.Scope.IsClientVisible());
}
