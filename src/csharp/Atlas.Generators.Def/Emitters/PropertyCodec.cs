using System.Text;

namespace Atlas.Generators.Def.Emitters;

// Serialise / deserialise a property's in-memory value against a given
// SpanWriter / SpanReader expression. Knows which properties are struct-
// typed (delegating to the generated struct's Serialize/Deserialize pair)
// and which are scalar (using DefTypeHelper's flat Read/Write methods).
//
// This helper exists so every emitter that writes property bytes — full
// state serialiser, per-audience delta serialiser, baseline snapshot,
// delta apply — agrees on how to encode a given property. Ad-hoc
// `writer.WriteInt32({field})` calls that forgot the struct branch would
// silently corrupt the wire stream (the Custom-id write path was 4 bytes
// of "int" regardless of what the field actually was).
internal static class PropertyCodec
{
    public static bool IsStruct(PropertyDefModel prop) =>
        prop.TypeRef?.Kind == PropertyDataKind.Struct;

    public static bool IsList(PropertyDefModel prop) =>
        prop.TypeRef?.Kind == PropertyDataKind.List;

    public static bool IsDict(PropertyDefModel prop) =>
        prop.TypeRef?.Kind == PropertyDataKind.Dict;

    // Produces the expression that writes `value` to the writer identified
    // by `writerVar` (typically "writer" or "bodyWriter"). Does not emit a
    // trailing `;` so callers can wrap with additional syntax.
    //
    // The struct path is `{value}.Serialize(ref writerVar)` — an instance
    // method call on the value expression. Callers that pass a field name
    // are fine; passing a by-value property access would invoke Serialize
    // on a defensive copy (C# ref-struct-on-value rules), which is still
    // semantically correct (Serialize is read-only) but an anti-pattern
    // worth catching. Today all callers pass field names (`_mainWeapon`);
    // revisit if that changes.
    public static string WriteExpr(PropertyDefModel prop, string writerVar, string value)
    {
        if (IsStruct(prop))
            return $"{value}.Serialize(ref {writerVar})";
        return $"{writerVar}.{DefTypeHelper.WriteMethod(prop.Type)}({value})";
    }

    // Produces the expression that reads a fresh value off `readerVar`.
    // Caller assigns the result to the backing field.
    public static string ReadExpr(PropertyDefModel prop, string readerVar)
    {
        if (IsStruct(prop))
            return $"{prop.TypeRef!.StructName}.Deserialize(ref {readerVar})";
        return $"{readerVar}.{DefTypeHelper.ReadMethod(prop.Type)}()";
    }

    // Element-level codec used by container emitters and struct fields.
    // Works directly from a DataTypeRefModel so container children that
    // are themselves structs / scalars / nested containers route through
    // the same codec the property-level helpers use.
    //
    // Nested inner containers (list inside list, list inside struct, dict
    // value that's a list, etc.) intentionally use plain .NET collections
    // (List<T>, Dictionary<K,V>) — NOT ObservableList / ObservableDict.
    // Dirty tracking exists only at the outermost property level; mutating
    // an inner container in-place won't mark the outer prop dirty. Scripts
    // mutate inner containers by re-assigning the outer slot (bag[3] =
    // newInner) which DOES trip the outer dirty.
    //
    // Single-expression variants (WriteElementExpr / ReadElementExpr) stay
    // around for callers that need a drop-in scalar / struct expression.
    // They reject container kinds — use EmitElementWrite / EmitElementRead
    // for those.

    public static string WriteElementExpr(DataTypeRefModel elemType, string writerVar, string value)
    {
        if (elemType.Kind == PropertyDataKind.Struct)
            return $"{value}.Serialize(ref {writerVar})";
        return $"{writerVar}.{WriteMethodForKind(elemType.Kind)}({value})";
    }

    public static string ReadElementExpr(DataTypeRefModel elemType, string readerVar)
    {
        if (elemType.Kind == PropertyDataKind.Struct)
            return $"{elemType.StructName}.Deserialize(ref {readerVar})";
        return $"{readerVar}.{ReadMethodForKind(elemType.Kind)}()";
    }

    // Fresh variable names for generated loops. Nested recursions share
    // one instance so sibling branches (key vs value in a dict) don't
    // collide on `__n0` / `__n0`.
    public sealed class NameGen
    {
        private int _n;
        public string Fresh(string prefix = "__n") => $"{prefix}{_n++}";
    }

    // Emits statements that write `valueExpr` to `writerVar`. Handles
    // every DataTypeRef kind recursively — scalar / struct / list / dict.
    // Inner containers are treated as plain .NET collections: `foreach
    // (var x in value)` works whether value is a List<T> or a struct
    // enumerator on IReadOnlyDictionary. Caller supplies a valueExpr that
    // evaluates to the container itself (not `.Items`).
    public static void EmitElementWrite(StringBuilder sb, DataTypeRefModel type,
                                        string writerVar, string valueExpr,
                                        string indent, NameGen names)
    {
        switch (type.Kind)
        {
            case PropertyDataKind.Struct:
                sb.AppendLine($"{indent}{valueExpr}.Serialize(ref {writerVar});");
                break;
            case PropertyDataKind.List:
            {
                var iter = names.Fresh();
                sb.AppendLine($"{indent}{writerVar}.WriteUInt16((ushort){valueExpr}.Count);");
                sb.AppendLine($"{indent}foreach (var {iter} in {valueExpr})");
                sb.AppendLine($"{indent}{{");
                EmitElementWrite(sb, type.Elem!, writerVar, iter, indent + "    ", names);
                sb.AppendLine($"{indent}}}");
                break;
            }
            case PropertyDataKind.Dict:
            {
                var kv = names.Fresh();
                sb.AppendLine($"{indent}{writerVar}.WriteUInt16((ushort){valueExpr}.Count);");
                sb.AppendLine($"{indent}foreach (var {kv} in {valueExpr})");
                sb.AppendLine($"{indent}{{");
                EmitElementWrite(sb, type.Key!, writerVar, $"{kv}.Key", indent + "    ", names);
                EmitElementWrite(sb, type.Elem!, writerVar, $"{kv}.Value", indent + "    ", names);
                sb.AppendLine($"{indent}}}");
                break;
            }
            default:
                sb.AppendLine($"{indent}{writerVar}.{WriteMethodForKind(type.Kind)}({valueExpr});");
                break;
        }
    }

    // Emits statements that decode one `type` value off readerVar and bind
    // it to a freshly declared local. Returns the local name.
    //
    // `plainContainers` switches list / dict reads from the Observable
    // wrappers (default — used for property-level state) to plain
    // List<T> / Dictionary<K,V> (used by struct fields, where value-type
    // copy semantics make the Observable rebind chain unsound).
    public static string EmitElementRead(StringBuilder sb, DataTypeRefModel type,
                                         string readerVar, string indent, NameGen names,
                                         bool plainContainers = false)
    {
        var v = names.Fresh();
        switch (type.Kind)
        {
            case PropertyDataKind.Struct:
                sb.AppendLine($"{indent}var {v} = {type.StructName}.Deserialize(ref {readerVar});");
                break;
            case PropertyDataKind.List:
            {
                var csType = plainContainers ? CSharpTypeForStructField(type) : CSharpTypeFor(type);
                var addCall = plainContainers ? "Add" : "AddWithoutDirty";
                var count = names.Fresh();
                var i = names.Fresh();
                sb.AppendLine($"{indent}var {count} = {readerVar}.ReadUInt16();");
                sb.AppendLine($"{indent}var {v} = new {csType}();");
                // Pre-size the backing buffer so a long inner doesn't
                // pay log(N) reallocations during AddWithoutDirty/Add.
                sb.AppendLine($"{indent}{v}.EnsureCapacity({count});");
                sb.AppendLine($"{indent}for (int {i} = 0; {i} < {count}; ++{i})");
                sb.AppendLine($"{indent}{{");
                var elem = EmitElementRead(sb, type.Elem!, readerVar, indent + "    ", names, plainContainers);
                sb.AppendLine($"{indent}    {v}.{addCall}({elem});");
                sb.AppendLine($"{indent}}}");
                break;
            }
            case PropertyDataKind.Dict:
            {
                var csType = plainContainers ? CSharpTypeForStructField(type) : CSharpTypeFor(type);
                var addCall = plainContainers ? null : "AddWithoutDirty";
                var count = names.Fresh();
                var i = names.Fresh();
                sb.AppendLine($"{indent}var {count} = {readerVar}.ReadUInt16();");
                sb.AppendLine($"{indent}var {v} = new {csType}();");
                sb.AppendLine($"{indent}{v}.EnsureCapacity({count});");
                sb.AppendLine($"{indent}for (int {i} = 0; {i} < {count}; ++{i})");
                sb.AppendLine($"{indent}{{");
                var key = EmitElementRead(sb, type.Key!, readerVar, indent + "    ", names, plainContainers);
                var val = EmitElementRead(sb, type.Elem!, readerVar, indent + "    ", names, plainContainers);
                if (addCall == null)
                    sb.AppendLine($"{indent}    {v}[{key}] = {val};");
                else
                    sb.AppendLine($"{indent}    {v}.{addCall}({key}, {val});");
                sb.AppendLine($"{indent}}}");
                break;
            }
            default:
                sb.AppendLine($"{indent}var {v} = {readerVar}.{ReadMethodForKind(type.Kind)}();");
                break;
        }
        return v;
    }

    // Resolves a DataTypeRef to the C# type used by property-level state.
    // Containers map to Observable wrappers so a script-side
    // `matrix[0].Add(99)` hits the inner ObservableList, fires its
    // MarkChildDirty hook, and the recursive serializer ships a targeted
    // op instead of re-shipping the whole slot.
    public static string CSharpTypeFor(DataTypeRefModel t) => t.Kind switch
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
        PropertyDataKind.Struct => t.StructName ?? "__UNRESOLVED_STRUCT__",
        PropertyDataKind.List =>
            $"global::Atlas.Observable.ObservableList<{CSharpTypeFor(t.Elem!)}>",
        PropertyDataKind.Dict =>
            $"global::Atlas.Observable.ObservableDict<{CSharpTypeFor(t.Key!)}, {CSharpTypeFor(t.Elem!)}>",
        _ => "__UNSUPPORTED_ELEMENT_TYPE__",
    };

    // Variant for struct fields. Struct is a value type — its container
    // fields can't safely participate in the Observable rebind chain
    // (rebinding via the IRebindableFields hook on a struct copy is a
    // separate roadmap item). Plain collections preserve P0 / P1
    // round-trip semantics; mutating an inner container in-place won't
    // dirty the owning struct slot, so scripts must reassign the slot
    // for changes to replicate.
    public static string CSharpTypeForStructField(DataTypeRefModel t) => t.Kind switch
    {
        PropertyDataKind.List =>
            $"global::System.Collections.Generic.List<{CSharpTypeForStructField(t.Elem!)}>",
        PropertyDataKind.Dict =>
            $"global::System.Collections.Generic.Dictionary<{CSharpTypeForStructField(t.Key!)}, {CSharpTypeForStructField(t.Elem!)}>",
        _ => CSharpTypeFor(t),
    };

    // Wire-width estimate per element. Feeds CompactOpLog's byte-aware
    // fallback decision. Variable-width kinds (string, bytes, struct
    // with var fields, nested containers) get conservative values
    // chosen to favour op-log retention rather than over-eager fallback.
    public static int EstimateWireWidth(DataTypeRefModel t) => t.Kind switch
    {
        PropertyDataKind.Bool or PropertyDataKind.Int8 or PropertyDataKind.UInt8 => 1,
        PropertyDataKind.Int16 or PropertyDataKind.UInt16 => 2,
        PropertyDataKind.Int32 or PropertyDataKind.UInt32 or PropertyDataKind.Float => 4,
        PropertyDataKind.Int64 or PropertyDataKind.UInt64 or PropertyDataKind.Double => 8,
        PropertyDataKind.Vector3 => 12,
        PropertyDataKind.Quaternion => 16,
        PropertyDataKind.String or PropertyDataKind.Bytes => 8,
        PropertyDataKind.Struct => 16,
        PropertyDataKind.List or PropertyDataKind.Dict => 16,
        _ => 8,
    };

    public static string WriteMethodForKind(PropertyDataKind kind) => kind switch
    {
        PropertyDataKind.Bool => "WriteBool",
        PropertyDataKind.Int8 => "WriteInt8",
        PropertyDataKind.UInt8 => "WriteUInt8",
        PropertyDataKind.Int16 => "WriteInt16",
        PropertyDataKind.UInt16 => "WriteUInt16",
        PropertyDataKind.Int32 => "WriteInt32",
        PropertyDataKind.UInt32 => "WriteUInt32",
        PropertyDataKind.Int64 => "WriteInt64",
        PropertyDataKind.UInt64 => "WriteUInt64",
        PropertyDataKind.Float => "WriteFloat",
        PropertyDataKind.Double => "WriteDouble",
        PropertyDataKind.String => "WriteString",
        PropertyDataKind.Bytes => "WriteBytes",
        PropertyDataKind.Vector3 => "WriteVector3",
        PropertyDataKind.Quaternion => "WriteQuaternion",
        _ => "WriteInt32",
    };

    public static string ReadMethodForKind(PropertyDataKind kind) => kind switch
    {
        PropertyDataKind.Bool => "ReadBool",
        PropertyDataKind.Int8 => "ReadInt8",
        PropertyDataKind.UInt8 => "ReadUInt8",
        PropertyDataKind.Int16 => "ReadInt16",
        PropertyDataKind.UInt16 => "ReadUInt16",
        PropertyDataKind.Int32 => "ReadInt32",
        PropertyDataKind.UInt32 => "ReadUInt32",
        PropertyDataKind.Int64 => "ReadInt64",
        PropertyDataKind.UInt64 => "ReadUInt64",
        PropertyDataKind.Float => "ReadFloat",
        PropertyDataKind.Double => "ReadDouble",
        PropertyDataKind.String => "ReadString",
        PropertyDataKind.Bytes => "ReadBytes",
        PropertyDataKind.Vector3 => "ReadVector3",
        PropertyDataKind.Quaternion => "ReadQuaternion",
        _ => "ReadInt32",
    };

    // ---- List-property emit helpers (P1 integral-sync path) ------------
    //
    // list[T] can't fit into a single Write/Read expression — the codec
    // needs a count prefix + a loop. The entity's property getter
    // (`Titles`, not `_titles`) is used on both sides so lazy-init
    // happens exactly once per entity even if a delta arrives before any
    // script write triggered allocation.

    public static void EmitListWrite(StringBuilder sb, PropertyDefModel prop, string writerVar,
                                     string indent)
    {
        var elemType = prop.TypeRef!.Elem!;
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var names = new NameGen();
        sb.AppendLine($"{indent}{writerVar}.WriteUInt16((ushort){propName}.Count);");
        // Iterate the ObservableList directly, not via .Items.  ObservableList
        // exposes a struct GetEnumerator() returning List<T>.Enumerator —
        // taking the enumerator off the IReadOnlyList<T> interface that .Items
        // returns boxes one heap object per call (visible as the
        // `List`1[T].IEnumerable<T>.GetEnumerator()` frames in dotnet-trace
        // gc-verbose captures).
        sb.AppendLine($"{indent}foreach (var __v in {propName})");
        sb.AppendLine($"{indent}{{");
        EmitElementWrite(sb, elemType, writerVar, "__v", indent + "    ", names);
        sb.AppendLine($"{indent}}}");
    }

    public static void EmitListRead(StringBuilder sb, PropertyDefModel prop, string readerVar,
                                    string indent)
    {
        var elemType = prop.TypeRef!.Elem!;
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var names = new NameGen();
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    var __count = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}    var __list = {propName};");
        sb.AppendLine($"{indent}    __list.ClearWithoutDirty();");
        sb.AppendLine($"{indent}    __list.EnsureCapacity(__count);");
        sb.AppendLine($"{indent}    for (int __i = 0; __i < __count; ++__i)");
        sb.AppendLine($"{indent}    {{");
        var elem = EmitElementRead(sb, elemType, readerVar, indent + "        ", names);
        sb.AppendLine($"{indent}        __list.AddWithoutDirty({elem});");
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
    }

    // Read-and-discard variant: the other side may still have emitted list
    // bytes for a prop whose backing field we don't own (e.g. client reading
    // a base-scope payload during legacy fallback). We must still advance
    // the reader past the count + N elements.
    public static void EmitListReadDiscard(StringBuilder sb, PropertyDefModel prop,
                                           string readerVar, string indent)
    {
        var elemType = prop.TypeRef!.Elem!;
        var names = new NameGen();
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    var __count = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}    for (int __i = 0; __i < __count; ++__i)");
        sb.AppendLine($"{indent}    {{");
        EmitElementReadDiscard(sb, elemType, readerVar, indent + "        ", names);
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
    }

    // Advances the reader past one value of `type` without binding it to
    // a local. Used on sides that don't own the backing field (client
    // ctx reading a base-scope payload through the legacy fallback).
    public static void EmitElementReadDiscard(StringBuilder sb, DataTypeRefModel type,
                                              string readerVar, string indent, NameGen names)
    {
        switch (type.Kind)
        {
            case PropertyDataKind.Struct:
                sb.AppendLine($"{indent}_ = {type.StructName}.Deserialize(ref {readerVar});");
                break;
            case PropertyDataKind.List:
            {
                var count = names.Fresh();
                var i = names.Fresh();
                sb.AppendLine($"{indent}{{");
                sb.AppendLine($"{indent}    var {count} = {readerVar}.ReadUInt16();");
                sb.AppendLine($"{indent}    for (int {i} = 0; {i} < {count}; ++{i})");
                sb.AppendLine($"{indent}    {{");
                EmitElementReadDiscard(sb, type.Elem!, readerVar, indent + "        ", names);
                sb.AppendLine($"{indent}    }}");
                sb.AppendLine($"{indent}}}");
                break;
            }
            case PropertyDataKind.Dict:
            {
                var count = names.Fresh();
                var i = names.Fresh();
                sb.AppendLine($"{indent}{{");
                sb.AppendLine($"{indent}    var {count} = {readerVar}.ReadUInt16();");
                sb.AppendLine($"{indent}    for (int {i} = 0; {i} < {count}; ++{i})");
                sb.AppendLine($"{indent}    {{");
                EmitElementReadDiscard(sb, type.Key!, readerVar, indent + "        ", names);
                EmitElementReadDiscard(sb, type.Elem!, readerVar, indent + "        ", names);
                sb.AppendLine($"{indent}    }}");
                sb.AppendLine($"{indent}}}");
                break;
            }
            default:
                sb.AppendLine($"{indent}_ = {readerVar}.{ReadMethodForKind(type.Kind)}();");
                break;
        }
    }

    // ---- Dict-property emit helpers ------------------------------------
    //
    // Wire: [u16 count][K v]* — each key-value pair encoded back-to-back
    // via the element codec. DefTypeExprParser restricts keys to scalar
    // kinds (string + fixed-width ints) so the key path never routes
    // through struct.Serialize.

    public static void EmitDictWrite(StringBuilder sb, PropertyDefModel prop, string writerVar,
                                     string indent)
    {
        var keyType = prop.TypeRef!.Key!;
        var elemType = prop.TypeRef!.Elem!;
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var names = new NameGen();
        sb.AppendLine($"{indent}{writerVar}.WriteUInt16((ushort){propName}.Count);");
        // Iterate the ObservableDict directly, not via .Items.  Same
        // boxing rationale as EmitListWrite — ObservableDict's struct
        // Dictionary<K,V>.Enumerator stays on the stack when iterated
        // through the concrete type.
        sb.AppendLine($"{indent}foreach (var __kv in {propName})");
        sb.AppendLine($"{indent}{{");
        EmitElementWrite(sb, keyType, writerVar, "__kv.Key", indent + "    ", names);
        EmitElementWrite(sb, elemType, writerVar, "__kv.Value", indent + "    ", names);
        sb.AppendLine($"{indent}}}");
    }

    public static void EmitDictRead(StringBuilder sb, PropertyDefModel prop, string readerVar,
                                    string indent)
    {
        var keyType = prop.TypeRef!.Key!;
        var elemType = prop.TypeRef!.Elem!;
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var names = new NameGen();
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    var __count = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}    var __dict = {propName};");
        sb.AppendLine($"{indent}    __dict.ClearWithoutDirty();");
        sb.AppendLine($"{indent}    for (int __i = 0; __i < __count; ++__i)");
        sb.AppendLine($"{indent}    {{");
        var k = EmitElementRead(sb, keyType, readerVar, indent + "        ", names);
        var v = EmitElementRead(sb, elemType, readerVar, indent + "        ", names);
        sb.AppendLine($"{indent}        __dict.AddWithoutDirty({k}, {v});");
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
    }

    public static void EmitDictReadDiscard(StringBuilder sb, PropertyDefModel prop,
                                           string readerVar, string indent)
    {
        var keyType = prop.TypeRef!.Key!;
        var elemType = prop.TypeRef!.Elem!;
        var names = new NameGen();
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    var __count = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}    for (int __i = 0; __i < __count; ++__i)");
        sb.AppendLine($"{indent}    {{");
        EmitElementReadDiscard(sb, keyType, readerVar, indent + "        ", names);
        EmitElementReadDiscard(sb, elemType, readerVar, indent + "        ", names);
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
    }

    // ---- Op-log delta emit helpers -------------------------------------
    //
    // Used by DeltaSyncEmitter on the delta path. Wire shape per
    // container property is recursive:
    //
    //   [u16 thisLevelOpCount][ops at this level]
    //   [u16 childDirtySlotOrKeyCount]
    //   foreach dirty child:
    //     [slot u16 | key...]
    //     <<recurse into child container>>
    //
    // The child section only appears when the element type is itself a
    // container — scalar / struct elements have no op log to recurse
    // into. (Struct elements with Observable fields stay plain today;
    // when that's enabled, child dirty must be converted to top-level
    // kSet ops upstream of this helper.)
    //
    // Full-state Serialize / Deserialize keeps the integral encoding —
    // baseline has no prior state to replay ops against.

    public static void EmitListOpLogWrite(StringBuilder sb, PropertyDefModel prop,
                                          string writerVar, string indent)
    {
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var names = new NameGen();
        EmitContainerOpLogWriteRecursive(sb, prop.TypeRef!, writerVar, propName, indent, names);
    }

    public static void EmitListOpLogRead(StringBuilder sb, PropertyDefModel prop,
                                         string readerVar, string indent)
    {
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var names = new NameGen();
        EmitContainerOpLogReadRecursive(sb, prop.TypeRef!, readerVar, propName, indent, names);
    }

    // Canonical recursive write — handles both list and dict, any nesting.
    // `containerExpr` must evaluate to the ObservableList / ObservableDict
    // instance (e.g., `Matrix` or `Matrix.Items[__slot]`).
    private static void EmitContainerOpLogWriteRecursive(
        StringBuilder sb, DataTypeRefModel type, string writerVar,
        string containerExpr, string indent, NameGen names)
    {
        var compactArgs = type.Kind == PropertyDataKind.List
            ? $"elementWireBytes: {EstimateWireWidth(type.Elem!)}"
            : $"keyWireBytes: {EstimateWireWidth(type.Key!)}, valueWireBytes: {EstimateWireWidth(type.Elem!)}";
        sb.AppendLine($"{indent}{containerExpr}.CompactOpLog({compactArgs});");

        // Dirty count uses packed varint — most ticks have 0 dirty
        // children (1 byte) instead of a fixed u16 (2 bytes).
        if (type.Kind == PropertyDataKind.List)
        {
            EmitListLevelWrite(sb, type, writerVar, containerExpr, indent, names);
            if (IsContainer(type.Elem!))
            {
                var slotVar = names.Fresh();
                sb.AppendLine($"{indent}{writerVar}.WritePackedUInt32((uint){containerExpr}.ChildDirtySlots.Count);");
                sb.AppendLine($"{indent}foreach (var {slotVar} in {containerExpr}.ChildDirtySlots)");
                sb.AppendLine($"{indent}{{");
                sb.AppendLine($"{indent}    {writerVar}.WriteUInt16((ushort){slotVar});");
                EmitContainerOpLogWriteRecursive(sb, type.Elem!, writerVar,
                                                 $"{containerExpr}.Items[{slotVar}]",
                                                 indent + "    ", names);
                sb.AppendLine($"{indent}}}");
                sb.AppendLine($"{indent}{containerExpr}.ClearChildDirty();");
            }
        }
        else if (type.Kind == PropertyDataKind.Dict)
        {
            EmitDictLevelWrite(sb, type, writerVar, containerExpr, indent, names);
            if (IsContainer(type.Elem!))
            {
                var keyVar = names.Fresh();
                sb.AppendLine($"{indent}{writerVar}.WritePackedUInt32((uint){containerExpr}.ChildDirtyKeys.Count);");
                sb.AppendLine($"{indent}foreach (var {keyVar} in {containerExpr}.ChildDirtyKeys)");
                sb.AppendLine($"{indent}{{");
                EmitElementWrite(sb, type.Key!, writerVar, keyVar, indent + "    ", names);
                EmitContainerOpLogWriteRecursive(sb, type.Elem!, writerVar,
                                                 $"{containerExpr}.Items[{keyVar}]",
                                                 indent + "    ", names);
                sb.AppendLine($"{indent}}}");
                sb.AppendLine($"{indent}{containerExpr}.ClearChildDirty();");
            }
        }
    }

    private static void EmitContainerOpLogReadRecursive(
        StringBuilder sb, DataTypeRefModel type, string readerVar,
        string containerExpr, string indent, NameGen names)
    {
        // Everything we emit here is the body of an `if (flags & PropBit)
        // != 0)` at the entity level — must live in one enclosing `{}` so
        // it all binds to the if and not just the first sub-statement.
        sb.AppendLine($"{indent}{{");
        var body = indent + "    ";

        if (type.Kind == PropertyDataKind.List)
        {
            EmitListLevelRead(sb, type, readerVar, containerExpr, body, names);
            if (IsContainer(type.Elem!))
            {
                var dirtyCount = names.Fresh();
                var d = names.Fresh();
                var slotVar = names.Fresh();
                sb.AppendLine($"{body}var {dirtyCount} = {readerVar}.ReadPackedUInt32();");
                sb.AppendLine($"{body}for (uint {d} = 0; {d} < {dirtyCount}; ++{d})");
                sb.AppendLine($"{body}{{");
                sb.AppendLine($"{body}    var {slotVar} = {readerVar}.ReadUInt16();");
                EmitContainerOpLogReadRecursive(sb, type.Elem!, readerVar,
                                                $"{containerExpr}.Items[{slotVar}]",
                                                body + "    ", names);
                sb.AppendLine($"{body}}}");
            }
        }
        else if (type.Kind == PropertyDataKind.Dict)
        {
            EmitDictLevelRead(sb, type, readerVar, containerExpr, body, names);
            if (IsContainer(type.Elem!))
            {
                var dirtyCount = names.Fresh();
                var d = names.Fresh();
                sb.AppendLine($"{body}var {dirtyCount} = {readerVar}.ReadPackedUInt32();");
                sb.AppendLine($"{body}for (uint {d} = 0; {d} < {dirtyCount}; ++{d})");
                sb.AppendLine($"{body}{{");
                var keyLocal = EmitElementRead(sb, type.Key!, readerVar, body + "    ", names);
                EmitContainerOpLogReadRecursive(sb, type.Elem!, readerVar,
                                                $"{containerExpr}.Items[{keyLocal}]",
                                                body + "    ", names);
                sb.AppendLine($"{body}}}");
            }
        }

        sb.AppendLine($"{indent}}}");
    }

    private static bool IsContainer(DataTypeRefModel t) =>
        t.Kind == PropertyDataKind.List || t.Kind == PropertyDataKind.Dict;

    // Emit this-level writes for a list — the ops that live directly on
    // `containerExpr`'s own op log (not its children's).
    private static void EmitListLevelWrite(StringBuilder sb, DataTypeRefModel type,
                                           string writerVar, string containerExpr,
                                           string indent, NameGen names)
    {
        var opVar = names.Fresh();
        var jVar = names.Fresh();
        sb.AppendLine($"{indent}{writerVar}.WriteUInt16((ushort){containerExpr}.Ops.Count);");
        sb.AppendLine($"{indent}foreach (var {opVar} in {containerExpr}.Ops)");
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    {writerVar}.WriteByte((byte){opVar}.Kind);");
        sb.AppendLine($"{indent}    switch ({opVar}.Kind)");
        sb.AppendLine($"{indent}    {{");
        sb.AppendLine($"{indent}        case global::Atlas.Observable.OpKind.Set:");
        sb.AppendLine($"{indent}            {writerVar}.WriteUInt16((ushort){opVar}.Start);");
        EmitElementWrite(sb, type.Elem!, writerVar,
                         $"{containerExpr}.OpValues[{opVar}.ValueOffset]",
                         indent + "            ", names);
        sb.AppendLine($"{indent}            break;");
        sb.AppendLine($"{indent}        case global::Atlas.Observable.OpKind.ListSplice:");
        sb.AppendLine($"{indent}            {writerVar}.WriteUInt16((ushort){opVar}.Start);");
        sb.AppendLine($"{indent}            {writerVar}.WriteUInt16((ushort){opVar}.End);");
        sb.AppendLine($"{indent}            {writerVar}.WriteUInt16((ushort){opVar}.ValueCount);");
        sb.AppendLine($"{indent}            for (int {jVar} = 0; {jVar} < {opVar}.ValueCount; ++{jVar})");
        sb.AppendLine($"{indent}            {{");
        EmitElementWrite(sb, type.Elem!, writerVar,
                         $"{containerExpr}.OpValues[{opVar}.ValueOffset + {jVar}]",
                         indent + "                ", names);
        sb.AppendLine($"{indent}            }}");
        sb.AppendLine($"{indent}            break;");
        sb.AppendLine($"{indent}        case global::Atlas.Observable.OpKind.Clear:");
        sb.AppendLine($"{indent}            break;");
        // Field-mode struct elements emit kStructFieldSet via ItemAt;
        // payload is just the changed field's bytes via the struct's
        // generated __WriteFieldOp dispatcher.
        if (type.Elem!.Kind == PropertyDataKind.Struct)
        {
            sb.AppendLine($"{indent}        case global::Atlas.Observable.OpKind.StructFieldSet:");
            sb.AppendLine($"{indent}            {writerVar}.WriteUInt16((ushort){opVar}.Start);");
            sb.AppendLine($"{indent}            {writerVar}.WriteByte((byte){opVar}.End);");
            sb.AppendLine($"{indent}            {containerExpr}.OpValues[{opVar}.ValueOffset].__WriteFieldOp(ref {writerVar}, (byte){opVar}.End);");
            sb.AppendLine($"{indent}            break;");
        }
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
        sb.AppendLine($"{indent}{containerExpr}.ClearOpLog();");
    }

    private static void EmitListLevelRead(StringBuilder sb, DataTypeRefModel type,
                                          string readerVar, string containerExpr,
                                          string indent, NameGen names)
    {
        var opCount = names.Fresh();
        var i = names.Fresh();
        var kindVar = names.Fresh();
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    var {opCount} = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}    for (int {i} = 0; {i} < {opCount}; ++{i})");
        sb.AppendLine($"{indent}    {{");
        sb.AppendLine($"{indent}        var {kindVar} = (global::Atlas.Observable.OpKind){readerVar}.ReadByte();");
        sb.AppendLine($"{indent}        switch ({kindVar})");
        sb.AppendLine($"{indent}        {{");
        sb.AppendLine($"{indent}            case global::Atlas.Observable.OpKind.Set:");
        sb.AppendLine($"{indent}            {{");
        var setIdx = names.Fresh();
        sb.AppendLine($"{indent}                var {setIdx} = {readerVar}.ReadUInt16();");
        var setVal = EmitElementRead(sb, type.Elem!, readerVar, indent + "                ", names);
        sb.AppendLine($"{indent}                {containerExpr}.SetWithoutDirty({setIdx}, {setVal});");
        sb.AppendLine($"{indent}                break;");
        sb.AppendLine($"{indent}            }}");
        sb.AppendLine($"{indent}            case global::Atlas.Observable.OpKind.ListSplice:");
        sb.AppendLine($"{indent}            {{");
        var start = names.Fresh();
        var end = names.Fresh();
        var vcount = names.Fresh();
        var j = names.Fresh();
        sb.AppendLine($"{indent}                var {start} = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}                var {end} = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}                var {vcount} = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}                {containerExpr}.RemoveRangeWithoutDirty({start}, {end});");
        sb.AppendLine($"{indent}                for (int {j} = 0; {j} < {vcount}; ++{j})");
        sb.AppendLine($"{indent}                {{");
        var spliceVal = EmitElementRead(sb, type.Elem!, readerVar, indent + "                    ", names);
        sb.AppendLine($"{indent}                    {containerExpr}.InsertWithoutDirty({start} + {j}, {spliceVal});");
        sb.AppendLine($"{indent}                }}");
        sb.AppendLine($"{indent}                break;");
        sb.AppendLine($"{indent}            }}");
        sb.AppendLine($"{indent}            case global::Atlas.Observable.OpKind.Clear:");
        sb.AppendLine($"{indent}                {containerExpr}.ClearWithoutDirty();");
        sb.AppendLine($"{indent}                break;");
        if (type.Elem!.Kind == PropertyDataKind.Struct)
        {
            var sfsSlot = names.Fresh();
            var sfsField = names.Fresh();
            sb.AppendLine($"{indent}            case global::Atlas.Observable.OpKind.StructFieldSet:");
            sb.AppendLine($"{indent}            {{");
            sb.AppendLine($"{indent}                var {sfsSlot} = {readerVar}.ReadUInt16();");
            sb.AppendLine($"{indent}                var {sfsField} = {readerVar}.ReadByte();");
            sb.AppendLine($"{indent}                var __cur = {containerExpr}.Items[{sfsSlot}];");
            sb.AppendLine($"{indent}                {containerExpr}.SetWithoutDirty({sfsSlot}, {type.Elem.StructName}.__ApplyFieldOp(ref {readerVar}, {sfsField}, __cur));");
            sb.AppendLine($"{indent}                break;");
            sb.AppendLine($"{indent}            }}");
        }
        sb.AppendLine($"{indent}        }}");
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
    }

    // Read-and-discard for lists on the op-log wire: the other side may
    // have emitted ops for a property whose backing field we don't own.
    public static void EmitListOpLogReadDiscard(StringBuilder sb, PropertyDefModel prop,
                                                string readerVar, string indent)
    {
        var elemType = prop.TypeRef!.Elem!;
        var names = new NameGen();
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    var __opCount = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}    for (int __i = 0; __i < __opCount; ++__i)");
        sb.AppendLine($"{indent}    {{");
        sb.AppendLine($"{indent}        var __kind = (global::Atlas.Observable.OpKind){readerVar}.ReadByte();");
        sb.AppendLine($"{indent}        if (__kind == global::Atlas.Observable.OpKind.Set)");
        sb.AppendLine($"{indent}        {{");
        sb.AppendLine($"{indent}            _ = {readerVar}.ReadUInt16();");
        EmitElementReadDiscard(sb, elemType, readerVar, indent + "            ", names);
        sb.AppendLine($"{indent}        }}");
        sb.AppendLine($"{indent}        else if (__kind == global::Atlas.Observable.OpKind.ListSplice)");
        sb.AppendLine($"{indent}        {{");
        sb.AppendLine($"{indent}            _ = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}            _ = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}            var __c = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}            for (int __j = 0; __j < __c; ++__j)");
        sb.AppendLine($"{indent}            {{");
        EmitElementReadDiscard(sb, elemType, readerVar, indent + "                ", names);
        sb.AppendLine($"{indent}            }}");
        sb.AppendLine($"{indent}        }}");
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
    }

    public static void EmitDictOpLogWrite(StringBuilder sb, PropertyDefModel prop,
                                          string writerVar, string indent)
    {
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var names = new NameGen();
        EmitContainerOpLogWriteRecursive(sb, prop.TypeRef!, writerVar, propName, indent, names);
    }

    public static void EmitDictOpLogRead(StringBuilder sb, PropertyDefModel prop,
                                         string readerVar, string indent)
    {
        var propName = DefTypeHelper.ToPropertyName(prop.Name);
        var names = new NameGen();
        EmitContainerOpLogReadRecursive(sb, prop.TypeRef!, readerVar, propName, indent, names);
    }

    private static void EmitDictLevelWrite(StringBuilder sb, DataTypeRefModel type,
                                           string writerVar, string containerExpr,
                                           string indent, NameGen names)
    {
        var opVar = names.Fresh();
        sb.AppendLine($"{indent}{writerVar}.WriteUInt16((ushort){containerExpr}.Ops.Count);");
        sb.AppendLine($"{indent}foreach (var {opVar} in {containerExpr}.Ops)");
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    {writerVar}.WriteByte((byte){opVar}.Kind);");
        sb.AppendLine($"{indent}    switch ({opVar}.Kind)");
        sb.AppendLine($"{indent}    {{");
        sb.AppendLine($"{indent}        case global::Atlas.Observable.OpKind.DictSet:");
        EmitElementWrite(sb, type.Key!, writerVar, $"{containerExpr}.OpKeys[{opVar}.KeyIndex]",
                         indent + "            ", names);
        EmitElementWrite(sb, type.Elem!, writerVar, $"{containerExpr}.OpValues[{opVar}.ValueIndex]",
                         indent + "            ", names);
        sb.AppendLine($"{indent}            break;");
        sb.AppendLine($"{indent}        case global::Atlas.Observable.OpKind.DictErase:");
        EmitElementWrite(sb, type.Key!, writerVar, $"{containerExpr}.OpKeys[{opVar}.KeyIndex]",
                         indent + "            ", names);
        sb.AppendLine($"{indent}            break;");
        sb.AppendLine($"{indent}        case global::Atlas.Observable.OpKind.Clear:");
        sb.AppendLine($"{indent}            break;");
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
        sb.AppendLine($"{indent}{containerExpr}.ClearOpLog();");
    }

    private static void EmitDictLevelRead(StringBuilder sb, DataTypeRefModel type,
                                          string readerVar, string containerExpr,
                                          string indent, NameGen names)
    {
        var opCount = names.Fresh();
        var i = names.Fresh();
        var kindVar = names.Fresh();
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    var {opCount} = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}    for (int {i} = 0; {i} < {opCount}; ++{i})");
        sb.AppendLine($"{indent}    {{");
        sb.AppendLine($"{indent}        var {kindVar} = (global::Atlas.Observable.OpKind){readerVar}.ReadByte();");
        sb.AppendLine($"{indent}        switch ({kindVar})");
        sb.AppendLine($"{indent}        {{");
        sb.AppendLine($"{indent}            case global::Atlas.Observable.OpKind.DictSet:");
        sb.AppendLine($"{indent}            {{");
        var setK = EmitElementRead(sb, type.Key!, readerVar, indent + "                ", names);
        var setV = EmitElementRead(sb, type.Elem!, readerVar, indent + "                ", names);
        sb.AppendLine($"{indent}                {containerExpr}.AddWithoutDirty({setK}, {setV});");
        sb.AppendLine($"{indent}                break;");
        sb.AppendLine($"{indent}            }}");
        sb.AppendLine($"{indent}            case global::Atlas.Observable.OpKind.DictErase:");
        sb.AppendLine($"{indent}            {{");
        var eraseK = EmitElementRead(sb, type.Key!, readerVar, indent + "                ", names);
        sb.AppendLine($"{indent}                {containerExpr}.RemoveWithoutDirty({eraseK});");
        sb.AppendLine($"{indent}                break;");
        sb.AppendLine($"{indent}            }}");
        sb.AppendLine($"{indent}            case global::Atlas.Observable.OpKind.Clear:");
        sb.AppendLine($"{indent}                {containerExpr}.ClearWithoutDirty();");
        sb.AppendLine($"{indent}                break;");
        sb.AppendLine($"{indent}        }}");
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
    }

    public static void EmitDictOpLogReadDiscard(StringBuilder sb, PropertyDefModel prop,
                                                string readerVar, string indent)
    {
        var keyType = prop.TypeRef!.Key!;
        var elemType = prop.TypeRef!.Elem!;
        var names = new NameGen();
        sb.AppendLine($"{indent}{{");
        sb.AppendLine($"{indent}    var __opCount = {readerVar}.ReadUInt16();");
        sb.AppendLine($"{indent}    for (int __i = 0; __i < __opCount; ++__i)");
        sb.AppendLine($"{indent}    {{");
        sb.AppendLine($"{indent}        var __kind = (global::Atlas.Observable.OpKind){readerVar}.ReadByte();");
        sb.AppendLine($"{indent}        if (__kind == global::Atlas.Observable.OpKind.DictSet)");
        sb.AppendLine($"{indent}        {{");
        EmitElementReadDiscard(sb, keyType, readerVar, indent + "            ", names);
        EmitElementReadDiscard(sb, elemType, readerVar, indent + "            ", names);
        sb.AppendLine($"{indent}        }}");
        sb.AppendLine($"{indent}        else if (__kind == global::Atlas.Observable.OpKind.DictErase)");
        sb.AppendLine($"{indent}        {{");
        EmitElementReadDiscard(sb, keyType, readerVar, indent + "            ", names);
        sb.AppendLine($"{indent}        }}");
        sb.AppendLine($"{indent}    }}");
        sb.AppendLine($"{indent}}}");
    }
}
