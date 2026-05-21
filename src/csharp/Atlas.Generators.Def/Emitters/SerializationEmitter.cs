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
            // Synced component slots ride Cell-side Serialize so Offload
            // preserves WeaponId / Equipment state etc. Base + Server too —
            // future cellBackup may stitch them through.
            var components = ctx == ProcessContext.Client
                ? new List<ComponentDefModel>()
                : def.Components
                    .Where(c => c.Locality == ComponentLocality.Synced && c.SlotIdx > 0)
                    .ToList();
            EmitSerialize(sb, sideProps, components);
            sb.AppendLine();
        }

        // Deserialize reads the matching wire format. The read-and-discard
        // branch in EmitDeserialize is dead for Client ctx today (the side
        // filter already excludes non-visible props) but kept as a harmless
        // passthrough until the wire-format layer is simplified.
        var deserComponents = ctx == ProcessContext.Client
            ? new List<ComponentDefModel>()
            : def.Components
                .Where(c => c.Locality == ComponentLocality.Synced && c.SlotIdx > 0)
                .ToList();
        EmitDeserialize(sb, sideProps, ctx, deserComponents);

        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitSerialize(StringBuilder sb, List<PropertyDefModel> props,
                                       List<ComponentDefModel> components)
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

        // Component section: per-slot (slot_idx:u8, payload_len:u16, payload)
        // tuples after the entity body. Reader checks Remaining() before
        // parsing so pre-component-section blobs deserialize unchanged.
        if (components.Count > 0)
        {
            sb.AppendLine("        byte componentCount = 0;");
            sb.AppendLine("        if (_replicated != null)");
            sb.AppendLine("        {");
            sb.AppendLine("            for (int i = 0; i < _replicated.Length; i++)");
            sb.AppendLine("                if (_replicated[i] != null) componentCount++;");
            sb.AppendLine("        }");
            sb.AppendLine("        writer.WriteByte(componentCount);");
            sb.AppendLine("        if (_replicated != null && componentCount > 0)");
            sb.AppendLine("        {");
            sb.AppendLine("            var compWriter = new SpanWriter(64);");
            sb.AppendLine("            try");
            sb.AppendLine("            {");
            sb.AppendLine("                for (int i = 0; i < _replicated.Length; i++)");
            sb.AppendLine("                {");
            sb.AppendLine("                    if (_replicated[i] is { } c)");
            sb.AppendLine("                    {");
            sb.AppendLine("                        compWriter.Reset();");
            sb.AppendLine("                        c.SerializeFull(ref compWriter);");
            sb.AppendLine("                        writer.WriteByte((byte)i);");
            sb.AppendLine("                        writer.WriteUInt16((ushort)compWriter.Length);");
            sb.AppendLine("                        writer.WriteRawBytes(compWriter.WrittenSpan);");
            sb.AppendLine("                    }");
            sb.AppendLine("                }");
            sb.AppendLine("            }");
            sb.AppendLine("            finally { compWriter.Dispose(); }");
            sb.AppendLine("        }");
        }
        sb.AppendLine("    }");
    }

    private static void EmitDeserialize(StringBuilder sb, List<PropertyDefModel> props,
                                         ProcessContext ctx, List<ComponentDefModel> components)
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

        // Optional component section — back-compat: blobs from before component
        // offload preservation have no trailing bytes after the entity body.
        if (components.Count > 0)
        {
            sb.AppendLine("        if (reader.Remaining > 0)");
            sb.AppendLine("        {");
            sb.AppendLine("            byte componentCount = reader.ReadByte();");
            sb.AppendLine("            for (int i = 0; i < componentCount; i++)");
            sb.AppendLine("            {");
            sb.AppendLine("                byte slot = reader.ReadByte();");
            sb.AppendLine("                ushort payloadLen = reader.ReadUInt16();");
            sb.AppendLine("                var payloadStart = reader.Position;");
            sb.AppendLine("                switch (slot)");
            sb.AppendLine("                {");
            foreach (var c in components)
            {
                sb.AppendLine($"                    case {c.SlotIdx}:");
                sb.AppendLine("                    {");
                sb.AppendLine($"                        var c{c.SlotIdx} = AddComponent<global::{ComponentEmitter.ComponentNamespace}.{c.TypeName}>();");
                sb.AppendLine($"                        c{c.SlotIdx}.DeserializeFull(ref reader);");
                sb.AppendLine($"                        c{c.SlotIdx}.ClearDirty();");
                sb.AppendLine("                        break;");
                sb.AppendLine("                    }");
            }
            sb.AppendLine("                }");
            sb.AppendLine("                var compConsumed = reader.Position - payloadStart;");
            sb.AppendLine("                if (compConsumed < payloadLen)");
            sb.AppendLine("                    reader.Advance(payloadLen - compConsumed);");
            sb.AppendLine("            }");
            sb.AppendLine("        }");
        }
        sb.AppendLine("    }");
    }

    private static bool IsClientVisible(PropertyScope scope) => scope.IsClientVisible();
}
