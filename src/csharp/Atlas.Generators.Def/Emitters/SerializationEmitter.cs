using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace Atlas.Generators.Def.Emitters;

/// <summary>
/// Generates Serialize/Deserialize methods and TypeName override from .def property definitions.
/// Uses version + fieldCount + bodyLength format for forward/backward compatibility.
/// </summary>
internal static class SerializationEmitter
{
    public static string Emit(EntityDefModel def, string className, string namespaceName,
                              ProcessContext ctx,
                              Dictionary<string, ushort>? typeIndexMap = null)
    {
        var sb = new StringBuilder();
        EmitterHeader.Write(sb, namespaceName);

        sb.AppendLine($"public partial class {className}");
        sb.AppendLine("{");
        sb.AppendLine($"    public override string TypeName => \"{def.Name}\";");
        // TypeId — used by component RPC stubs to compose rpc_id at runtime.
        // Falls back to 0 when typeIndexMap isn't provided (older callers /
        // tests); the runtime helper handles 0 as "unindexed entity".
        if (typeIndexMap != null && typeIndexMap.TryGetValue(def.Name, out var typeIndex))
            sb.AppendLine($"    public override ushort TypeId => {typeIndex};");
        sb.AppendLine();
        sb.AppendLine("    private const byte kSerializationVersion = 1;");
        sb.AppendLine();

        // ATLAS_DEF008: reserved-position props are skipped — PropertiesEmitter
        // emits no backing field, so referencing `_position` would not compile.
        // Base.Serialize writes DATA_BASE fields (to DBApp); Cell.Serialize
        // writes CELL_DATA fields (to offload bundle and BaseApp's cellBackup
        // bytes). BaseApp stitches base bytes + cell_backup_data_ for the
        // persistent blob so cell-scope persistent="true" round-trips cleanly.
        var sideProps = DefTypeHelper.PropertiesForContext(def.Properties, ctx)
            .Where(p => !p.IsReservedPosition).ToList();

        // Server/Base/Cell: generate Serialize (full state). Client never
        // originates state so it gets no Serialize.
        if (ctx != ProcessContext.Client)
        {
            EmitSerialize(sb, sideProps);
            sb.AppendLine();
        }

        // Deserialize reads the matching wire format. The read-and-discard
        // branch in EmitDeserialize is dead for Client ctx today (the side
        // filter already excludes non-visible props) but kept as a harmless
        // passthrough until the wire-format layer is simplified.
        EmitDeserialize(sb, sideProps, ctx);

        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitSerialize(StringBuilder sb, List<PropertyDefModel> props)
    {
        sb.AppendLine("    public override void Serialize(ref SpanWriter writer)");
        sb.AppendLine("    {");
        sb.AppendLine("        writer.WriteByte(kSerializationVersion);");
        sb.AppendLine($"        writer.WriteUInt16((ushort){props.Count});");
        sb.AppendLine("        var bodyWriter = new SpanWriter(256);");
        sb.AppendLine("        try");
        sb.AppendLine("        {");
        foreach (var prop in props)
        {
            if (PropertyCodec.IsList(prop))
            {
                PropertyCodec.EmitListWrite(sb, prop, "bodyWriter", "            ");
                continue;
            }
            if (PropertyCodec.IsDict(prop))
            {
                PropertyCodec.EmitDictWrite(sb, prop, "bodyWriter", "            ");
                continue;
            }
            var fieldName = DefTypeHelper.ToFieldName(prop.Name);
            sb.AppendLine($"            {PropertyCodec.WriteExpr(prop, "bodyWriter", fieldName)};");
        }
        sb.AppendLine("            writer.WriteUInt16((ushort)bodyWriter.Length);");
        sb.AppendLine("            writer.WriteRawBytes(bodyWriter.WrittenSpan);");
        sb.AppendLine("        }");
        sb.AppendLine("        finally { bodyWriter.Dispose(); }");
        sb.AppendLine("    }");
    }

    private static void EmitDeserialize(StringBuilder sb, List<PropertyDefModel> props, ProcessContext ctx)
    {
        sb.AppendLine("    public override void Deserialize(ref SpanReader reader)");
        sb.AppendLine("    {");
        sb.AppendLine("        var version = reader.ReadByte();");
        sb.AppendLine("        var fieldCount = reader.ReadUInt16();");
        sb.AppendLine("        var bodyLength = reader.ReadUInt16();");
        sb.AppendLine("        var bodyStart = reader.Position;");

        for (int i = 0; i < props.Count; i++)
        {
            bool hasField = ctx != ProcessContext.Client || IsClientVisible(props[i].Scope);

            if (PropertyCodec.IsList(props[i]))
            {
                sb.AppendLine($"        if (fieldCount > {i})");
                if (hasField)
                {
                    PropertyCodec.EmitListRead(sb, props[i], "reader", "        ");
                }
                else
                {
                    PropertyCodec.EmitListReadDiscard(sb, props[i], "reader", "        ");
                }
                continue;
            }

            if (PropertyCodec.IsDict(props[i]))
            {
                sb.AppendLine($"        if (fieldCount > {i})");
                if (hasField)
                {
                    PropertyCodec.EmitDictRead(sb, props[i], "reader", "        ");
                }
                else
                {
                    PropertyCodec.EmitDictReadDiscard(sb, props[i], "reader", "        ");
                }
                continue;
            }

            var readExpr = PropertyCodec.ReadExpr(props[i], "reader");
            if (hasField)
            {
                var fieldName = DefTypeHelper.ToFieldName(props[i].Name);
                sb.AppendLine($"        if (fieldCount > {i}) {fieldName} = {readExpr};");
            }
            else
            {
                // Read and discard — field doesn't exist in client context
                sb.AppendLine($"        if (fieldCount > {i}) _ = {readExpr};");
            }
        }

        sb.AppendLine("        var consumed = reader.Position - bodyStart;");
        sb.AppendLine("        if (consumed < bodyLength)");
        sb.AppendLine("            reader.Advance(bodyLength - consumed);");
        sb.AppendLine("    }");
    }

    private static bool IsClientVisible(PropertyScope scope) => scope.IsClientVisible();
}
