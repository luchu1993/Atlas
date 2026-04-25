using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace Atlas.Generators.Def.Emitters;

/// <summary>
/// Generates backing fields, public properties with dirty tracking, DirtyFlags enum,
/// and change callback hooks from .def property definitions.
/// </summary>
internal static class PropertiesEmitter
{
    public static string? Emit(EntityDefModel def, string className, string namespaceName,
                               ProcessContext ctx,
                               IReadOnlyDictionary<string, StructDefModel>? structsByName = null)
    {
        if (def.Properties.Count == 0) return null;

        // ATLAS_DEF008 (spans every ctx): a replicable `position` is a ghost
        // of ClientEntity.Position + the volatile channel, never a real field.
        var replicableProps = def.Properties
            .Where(p => p.Scope.IsClientVisible() && !p.IsReservedPosition)
            .ToList();

        // Backing fields live only on the side that OWNS the value (base XOR
        // cell). Client mirrors every visible prop; the legacy Server ctx is
        // the single-process fallback and emits everything.
        var props = DefTypeHelper.PropertiesForContext(def.Properties, ctx)
            .Where(p => !p.IsReservedPosition).ToList();
        if (props.Count == 0 && replicableProps.Count == 0) return null;

        // ReplicatedDirtyFlags enum MUST enumerate the full IsClientVisible
        // set in stable declaration order on every side — bit N must denote
        // the same property across sender, receiver and intermediate
        // observers, else the 0xF003 flag byte is uninterpretable. Enum does
        // NOT filter by ctx; backing fields do. Serializers skip props whose
        // field is missing on this side, leaving the unused bits at zero.
        var enumReplicableProps = replicableProps;
        var emitDirtyBacking = ctx != ProcessContext.Client && enumReplicableProps.Count > 0;

        var sb = new StringBuilder();
        EmitterHeader.Write(sb, namespaceName);
        sb.AppendLine("using System;");
        sb.AppendLine();

        sb.AppendLine($"partial class {className}");
        sb.AppendLine("{");

        if (enumReplicableProps.Count > 0)
        {
            EmitDirtyFlagsEnum(sb, enumReplicableProps);
            sb.AppendLine();
            if (emitDirtyBacking)
            {
                sb.AppendLine("    private ReplicatedDirtyFlags _dirtyFlags;");
                sb.AppendLine();
            }
        }

        // Backing fields
        foreach (var prop in props)
        {
            var csType = CSharpTypeFor(prop);
            var fieldName = DefTypeHelper.ToFieldName(prop.Name);
            if (IsListProp(prop))
            {
                // Lazy-cached ObservableList. `__`-prefix avoids colliding
                // with hand-written `_name` backing fields in partial files.
                sb.AppendLine($"    private {csType}? __{prop.Name}List;");
            }
            else if (IsDictProp(prop))
            {
                sb.AppendLine($"    private {csType}? __{prop.Name}Dict;");
            }
            else if (IsStructProp(prop))
            {
                sb.AppendLine($"    private {csType} {fieldName};");
            }
            else if (DefTypeHelper.NeedsFieldInitializer(prop.Type))
                sb.AppendLine($"    private {csType} {fieldName} = {DefTypeHelper.DefaultValue(prop.Type)};");
            else
                sb.AppendLine($"    private {csType} {fieldName};");
        }
        sb.AppendLine();

        // Change callback partial methods. Container props have no wholesale
        // "old → new" pair (mutations are per-element); a dedicated
        // container change hook is a P1 follow-up.
        foreach (var prop in props)
        {
            if (IsListProp(prop) || IsDictProp(prop)) continue;
            var csType = CSharpTypeFor(prop);
            var propName = DefTypeHelper.ToPropertyName(prop.Name);
            sb.AppendLine($"    partial void On{propName}Changed({csType} oldValue, {csType} newValue);");
        }
        sb.AppendLine();

        // Properties. Three setter shapes:
        //   * Struct-typed → MutRef accessor with field-level dirty hook
        //   * Server replicable scalar → dirty-bit + OnXxxChanged
        //   * Everything else           → bare assignment (no callback)
        // Client setters deliberately skip OnXxxChanged — only wire deltas
        // (ApplyReplicatedDelta) fire it, since local client writes are
        // edge cases (prediction, test fixtures) that shouldn't masquerade
        // as observed server transitions.
        foreach (var prop in props)
        {
            bool trackDirty = emitDirtyBacking && enumReplicableProps.Contains(prop);
            if (IsListProp(prop))
            {
                EmitListProperty(sb, prop, trackDirty);
                MaybeEmitListItemAtAccessor(sb, prop, structsByName);
            }
            else if (IsDictProp(prop))
            {
                EmitDictProperty(sb, prop, trackDirty);
            }
            else if (IsStructProp(prop) && structsByName != null
                && structsByName.TryGetValue(prop.TypeRef!.StructName!, out var structDef))
            {
                EmitStructProperty(sb, prop, structDef, className, trackDirty);
            }
            else
            {
                EmitProperty(sb, prop, trackDirty);
            }
            sb.AppendLine();
        }

        // IsDirty / ClearDirty reference _dirtyFlags — emit only when the
        // backing field exists (server side).
        if (emitDirtyBacking)
        {
            sb.AppendLine("    public bool IsDirty => _dirtyFlags != ReplicatedDirtyFlags.None;");
            sb.AppendLine();
            sb.AppendLine("    public void ClearDirty() => _dirtyFlags = ReplicatedDirtyFlags.None;");
        }

        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitDirtyFlagsEnum(StringBuilder sb, List<PropertyDefModel> props)
    {
        var (backingType, _) = GetFlagsTypeInfo(props.Count);
        sb.AppendLine("    [Flags]");
        sb.AppendLine($"    private enum ReplicatedDirtyFlags : {backingType}");
        sb.AppendLine("    {");
        sb.AppendLine("        None = 0,");
        for (int i = 0; i < props.Count; i++)
        {
            var comma = i < props.Count - 1 ? "," : "";
            var propName = DefTypeHelper.ToPropertyName(props[i].Name);
            // Use explicit numeric values to avoid uint-to-byte implicit conversion errors
            var flagValue = 1UL << i;
            sb.AppendLine($"        {propName} = {flagValue}{comma}");
        }
        sb.AppendLine("    }");
    }

    private static void EmitProperty(StringBuilder sb, PropertyDefModel prop, bool trackDirty)
    {
        var csType = DefTypeHelper.ToCSharpType(prop.Type);
        var fieldName = DefTypeHelper.ToFieldName(prop.Name);
        var propName = DefTypeHelper.ToPropertyName(prop.Name);

        sb.AppendLine($"    public {csType} {propName}");
        sb.AppendLine("    {");
        sb.AppendLine($"        get => {fieldName};");

        if (trackDirty)
        {
            sb.AppendLine("        set");
            sb.AppendLine("        {");
            sb.AppendLine($"            if ({fieldName} != value)");
            sb.AppendLine("            {");
            sb.AppendLine($"                var old = {fieldName};");
            sb.AppendLine($"                {fieldName} = value;");
            sb.AppendLine($"                _dirtyFlags |= ReplicatedDirtyFlags.{propName};");
            sb.AppendLine($"                On{propName}Changed(old, value);");
            sb.AppendLine("            }");
            sb.AppendLine("        }");
        }
        else
        {
            sb.AppendLine($"        set => {fieldName} = value;");
        }

        sb.AppendLine("    }");
    }

    private static bool IsReplicable(PropertyScope scope) => scope.IsClientVisible();

    private static bool IsStructProp(PropertyDefModel prop) =>
        prop.TypeRef?.Kind == PropertyDataKind.Struct;

    private static bool IsListProp(PropertyDefModel prop) =>
        prop.TypeRef?.Kind == PropertyDataKind.List;

    private static bool IsDictProp(PropertyDefModel prop) =>
        prop.TypeRef?.Kind == PropertyDataKind.Dict;

    // For struct-typed properties we use the user-declared struct name
    // (which lives in Atlas.Def thanks to the `using` at the top of the
    // generated file). For container properties the backing store is an
    // ObservableList<T> / ObservableDict<K,V>; element C# types route
    // through PropertyCodec so nested struct elements map to the generated
    // Atlas.Def.<Name>. Everything else defers to the flat .def → C#
    // mapping.
    private static string CSharpTypeFor(PropertyDefModel prop)
    {
        if (IsStructProp(prop)) return prop.TypeRef!.StructName!;
        if (IsListProp(prop))
        {
            var elemCs = PropertyCodec.CSharpTypeFor(prop.TypeRef!.Elem!);
            return $"global::Atlas.Observable.ObservableList<{elemCs}>";
        }
        if (IsDictProp(prop))
        {
            var keyCs = PropertyCodec.CSharpTypeFor(prop.TypeRef!.Key!);
            var elemCs = PropertyCodec.CSharpTypeFor(prop.TypeRef!.Elem!);
            return $"global::Atlas.Observable.ObservableDict<{keyCs}, {elemCs}>";
        }
        return DefTypeHelper.ToCSharpType(prop.Type);
    }

    // Shape of the emitted property (scripts write via the MutRef):
    //
    //   public MainWeaponMutRef MainWeapon => _mainWeaponMutRef ??= new(this);
    //   public ItemStack        MainWeaponValue => _mainWeapon;
    //
    //   public sealed class MainWeaponMutRef
    //   {
    //       private readonly Avatar _owner;
    //       internal MainWeaponMutRef(Avatar o) { _owner = o; }
    //       public int Id {
    //           get => _owner._mainWeapon.Id;
    //           set {
    //               if (_owner._mainWeapon.Id == value) return;
    //               _owner._mainWeapon.Id = value;
    //               _owner._dirtyFlags |= ReplicatedDirtyFlags.MainWeapon;
    //           }
    //       }
    //       public static implicit operator ItemStack(MainWeaponMutRef r)
    //           => r._owner._mainWeapon;
    //   }
    //
    // The MutRef is a reference type so `avatar.MainWeapon.Count = 5`
    // compiles: C# disallows the assignment when MainWeapon returns a
    // struct by value (CS1612 — setter would run on a temp). The class
    // is lazily constructed and cached on the entity so we pay the
    // allocation once per entity rather than per access. Nested inside
    // the entity class so the accessor can reach the private backing
    // field without exposing it.
    private static void EmitStructProperty(StringBuilder sb, PropertyDefModel prop,
                                           StructDefModel structDef, string className,
                                           bool trackDirty)
    {
        var structName = structDef.Name;
        var fieldName = DefTypeHelper.ToFieldName(prop.Name);
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var mutRefName = $"{propName}MutRef";
        // `__` prefix is reserved for generator-internal fields so user
        // hand-written backing fields (`_foo`, `_fooMutRef`, …) never collide.
        var mutRefField = $"__{char.ToLowerInvariant(propName[0])}{propName.Substring(1)}MutRef";

        sb.AppendLine($"    private {mutRefName}? {mutRefField};");
        sb.AppendLine($"    public {mutRefName} {propName} => {mutRefField} ??= new(this);");
        sb.AppendLine($"    public {structName} {propName}Value => {fieldName};");
        sb.AppendLine();
        sb.AppendLine($"    public sealed class {mutRefName}");
        sb.AppendLine("    {");
        sb.AppendLine($"        private readonly {className} _owner;");
        sb.AppendLine($"        internal {mutRefName}({className} owner) {{ _owner = owner; }}");
        sb.AppendLine();

        foreach (var f in structDef.Fields)
        {
            var fldCs = MapFieldCsType(f.Type);
            var fldName = DefTypeHelper.ToPropertyName(f.Name);
            sb.AppendLine($"        public {fldCs} {fldName}");
            sb.AppendLine("        {");
            sb.AppendLine($"            get => _owner.{fieldName}.{fldName};");
            sb.AppendLine("            set");
            sb.AppendLine("            {");
            sb.AppendLine($"                if (_owner.{fieldName}.{fldName} == value) return;");
            sb.AppendLine($"                _owner.{fieldName}.{fldName} = value;");
            if (trackDirty)
            {
                sb.AppendLine($"                _owner._dirtyFlags |= ReplicatedDirtyFlags.{propName};");
            }
            sb.AppendLine("            }");
            sb.AppendLine("        }");
        }
        sb.AppendLine();
        sb.AppendLine($"        public static implicit operator {structName}({mutRefName} r)");
        sb.AppendLine($"            => r._owner.{fieldName};");
        sb.AppendLine("    }");
    }

    // Shape of the emitted list property (scripts mutate via ObservableList<T>):
    //
    //   private ObservableList<int>? __titlesList;
    //   public ObservableList<int> Titles => __titlesList ??=
    //       new(() => _dirtyFlags |= ReplicatedDirtyFlags.Titles);
    //
    // On the client side _dirtyFlags doesn't exist — the callback is an
    // inert no-op since clients never originate replication. Apply-path
    // writes always use ClearWithoutDirty/AddWithoutDirty so the callback
    // never fires during server-to-client bounce anyway.
    private static void EmitListProperty(StringBuilder sb, PropertyDefModel prop, bool trackDirty)
    {
        var csType = CSharpTypeFor(prop);
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var backing = $"__{prop.Name}List";
        var markDirty = trackDirty
            ? $"() => _dirtyFlags |= ReplicatedDirtyFlags.{propName}"
            : "() => {}";
        sb.AppendLine($"    public {csType} {propName} => {backing} ??= new({markDirty});");
    }

    // For list<FieldStruct>, emit `<PropName>At(int)` returning the
    // generated ItemAt that lets scripts mutate one struct field in
    // place via `entity.PartyAt(0).Hp = 5`. Allocates per call (~24 B);
    // a shared per-entity cache would race when scripts reference two
    // ItemAts in the same statement.
    private static void MaybeEmitListItemAtAccessor(
        StringBuilder sb, PropertyDefModel prop,
        IReadOnlyDictionary<string, StructDefModel>? structsByName)
    {
        if (structsByName == null) return;
        var elemType = prop.TypeRef!.Elem!;
        if (elemType.Kind != PropertyDataKind.Struct) return;
        if (elemType.StructName == null) return;
        if (!structsByName.TryGetValue(elemType.StructName, out var structDef)) return;
        var decision = StructEmitter.DecideSyncMode(structDef);
        if (decision.Mode != StructSyncMode.Field) return;

        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var itemAtClass = $"global::{StructEmitter.StructNamespace}.{structDef.Name}ItemAt";
        sb.AppendLine($"    public {itemAtClass} {propName}At(int slot) => new(this.{propName}, slot);");
    }

    // Dict mirror of EmitListProperty — same lazy-cache + closure pattern.
    private static void EmitDictProperty(StringBuilder sb, PropertyDefModel prop, bool trackDirty)
    {
        var csType = CSharpTypeFor(prop);
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var backing = $"__{prop.Name}Dict";
        var markDirty = trackDirty
            ? $"() => _dirtyFlags |= ReplicatedDirtyFlags.{propName}"
            : "() => {}";
        sb.AppendLine($"    public {csType} {propName} => {backing} ??= new({markDirty});");
    }

    // Duplicate of StructEmitter's CSharpTypeOf — kept inlined here so we
    // don't reach into another emitter's internals. Containers / nested
    // structs aren't reachable today (the auto-sync rule forbids them as
    // struct fields); placeholder marker keeps future breakage loud.
    private static string MapFieldCsType(DataTypeRefModel t) => t.Kind switch
    {
        PropertyDataKind.Bool => "bool",
        PropertyDataKind.Int8 => "sbyte",
        PropertyDataKind.UInt8 => "byte",
        PropertyDataKind.Int16 => "short",
        PropertyDataKind.UInt16 => "ushort",
        PropertyDataKind.Int32 => "int",
        PropertyDataKind.UInt32 => "uint",
        PropertyDataKind.Int64 => "long",
        PropertyDataKind.UInt64 => "ulong",
        PropertyDataKind.Float => "float",
        PropertyDataKind.Double => "double",
        PropertyDataKind.String => "string",
        PropertyDataKind.Bytes => "byte[]",
        PropertyDataKind.Vector3 => "Atlas.DataTypes.Vector3",
        PropertyDataKind.Quaternion => "Atlas.DataTypes.Quaternion",
        _ => "__UNSUPPORTED_FIELD_TYPE__",
    };

    private static (string BackingType, string FlagSuffix) GetFlagsTypeInfo(int fieldCount)
    {
        if (fieldCount <= 8) return ("byte", "u");
        if (fieldCount <= 16) return ("ushort", "u");
        if (fieldCount <= 32) return ("uint", "u");
        return ("ulong", "UL");
    }
}
