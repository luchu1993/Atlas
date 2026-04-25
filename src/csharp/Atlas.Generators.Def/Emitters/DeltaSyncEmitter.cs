using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace Atlas.Generators.Def.Emitters;

/// <summary>
/// Generates scope-filtered delta sync methods from .def property definitions.
/// Uses the 8-value PropertyScope enum for precise scope filtering.
/// </summary>
internal static class DeltaSyncEmitter
{
    public static string? Emit(EntityDefModel def, string className, string namespaceName,
                               ProcessContext ctx)
    {
        // ATLAS_DEF008: a replicable `position` is excluded from the entire
        // replication pipeline (no enum bit, no delta, no snapshot) —
        // position rides the volatile channel via ClientEntity base.
        // ctx gating keeps the delta pipeline to props whose backing field
        // lives on THIS process (base vs cell); otherwise codegen references
        // fields that don't exist.
        var replicableProps = def.Properties
            .Where(p => p.Scope.IsClientVisible() && !p.IsReservedPosition &&
                        IsOnThisSide(p.Scope, ctx))
            .ToList();
        bool hasComponents = ctx != ProcessContext.Client &&
                             def.Components.Any(c => c.Locality == ComponentLocality.Synced);
        if (replicableProps.Count == 0 && !hasComponents) return null;

        // Client path: snapshot deserializers only (no dirty tracking, no
        // outbound serializers). Wire format mirrors the server's
        // SerializeForOwner/OtherClient: raw fields in declaration order
        // filtered by scope predicate — identical iteration both sides.
        if (ctx == ProcessContext.Client)
        {
            // Client may have NO own props but still need ApplyReplicatedDelta
            // to read sectionMask + dispatch component section. Components-
            // only entities still want a generated decoder.
            bool clientHasComponents = def.Components.Any(c => c.Locality == ComponentLocality.Synced);
            if (replicableProps.Count == 0 && !clientHasComponents) return null;
            return EmitClientSnapshotApplyMethods(def, className, namespaceName, replicableProps,
                                                  clientHasComponents);
        }

        var sb = new StringBuilder();
        EmitterHeader.Write(sb, namespaceName);
        sb.AppendLine($"partial class {className}");
        sb.AppendLine("{");

        if (replicableProps.Count > 0)
        {
            // Audience masks partition dirty flags for the replication pump.
            // Owner mask covers props visible to the owning client (OwnClient,
            // AllClients, CellPublicAndOwn, BaseAndClient); Other mask covers
            // observer-visible props (AllClients, OtherClients). CellPublicAndOwn
            // stays owner-only — revealing it to non-owners is a privacy bug.
            EmitAudienceMasks(sb, replicableProps);
            sb.AppendLine();

            // ApplyReplicatedDelta — decodes the wire delta (flag byte(s) +
            // conditional field reads). Wire-compatible with the per-audience
            // SerializeOwnerDelta / SerializeOtherDelta emitted below.
            EmitApplyReplicatedDelta(sb, replicableProps);
            sb.AppendLine();
        }

        // SerializeForOwnerClient — fields visible to owner client
        var ownerFields = replicableProps.Where(p => IsOwnerVisible(p.Scope)).ToList();
        if (ownerFields.Count > 0)
        {
            EmitScopeSerialize(sb, ownerFields, "SerializeForOwnerClient");
            sb.AppendLine();
        }

        // SerializeForOtherClients — fields visible to non-owner clients
        var otherFields = replicableProps.Where(p => IsOtherVisible(p.Scope)).ToList();
        if (otherFields.Count > 0)
        {
            EmitScopeSerialize(sb, otherFields, "SerializeForOtherClients");
            sb.AppendLine();
        }

        if (replicableProps.Count > 0)
        {
            // Per-audience delta serializers. Reliability is a transport-layer
            // concern (picked at BaseApp send path), not a serialization split.
            // hasComponents wires sectionMask bit 2 + a trailing component
            // section so the per-tick component delta rides the same channel.
            EmitAudienceDeltaSerializer(sb, replicableProps, "SerializeOwnerDelta",
                                        "OwnerVisibleScalarMask", "OwnerVisibleContainerMask",
                                        p => IsOwnerVisible(p.Scope),
                                        hasComponents, "HasOwnerDirtyComponent",
                                        "WriteOwnerComponentSection");
            sb.AppendLine();
            EmitAudienceDeltaSerializer(sb, replicableProps, "SerializeOtherDelta",
                                        "OtherVisibleScalarMask", "OtherVisibleContainerMask",
                                        p => IsOtherVisible(p.Scope),
                                        hasComponents, "HasOtherDirtyComponent",
                                        "WriteOtherComponentSection");
            sb.AppendLine();
        }
        else
        {
            // Components-only entity — no own _dirtyFlags / masks exist, so
            // SerializeXxxDelta emits sectionMask with bit 2 only and dispatches
            // straight into the component section.
            EmitComponentsOnlyAudienceDelta(sb, "SerializeOwnerDelta",
                                            "HasOwnerDirtyComponent", "WriteOwnerComponentSection");
            sb.AppendLine();
            EmitComponentsOnlyAudienceDelta(sb, "SerializeOtherDelta",
                                            "HasOtherDirtyComponent", "WriteOtherComponentSection");
            sb.AppendLine();
        }

        // Frame counters live in this generated partial so non-replicable
        // entities don't carry them.
        sb.AppendLine("    private ulong _eventSeq;");
        sb.AppendLine("    private ulong _volatileSeq;");
        sb.AppendLine();

        // BuildAndConsumeReplicationFrame — per-tick entry point for the
        // CellApp replication pump. Returns false when no dirty state, so
        // the caller skips both the allocation and the NativeApi hop.
        EmitBuildAndConsume(sb, ownerFields.Count > 0, otherFields.Count > 0,
                            hasComponents, replicableProps.Count > 0);

        sb.AppendLine("}");
        return sb.ToString();
    }

    // Emits a SerializeXxxDelta for entities whose only replicable state
    // lives in components: sectionMask byte with only bit 2 + component
    // section. No scalar / container blocks are generated because the
    // entity body has nothing to write at this level.
    private static void EmitComponentsOnlyAudienceDelta(StringBuilder sb, string methodName,
                                                        string componentDirtyPredicate,
                                                        string componentSectionWriter)
    {
        sb.AppendLine($"    public void {methodName}(ref SpanWriter writer)");
        sb.AppendLine("    {");
        sb.AppendLine($"        bool hasComponents = {componentDirtyPredicate}();");
        sb.AppendLine("        byte sectionMask = 0;");
        sb.AppendLine("        if (hasComponents) sectionMask |= 0x04;");
        sb.AppendLine("        writer.WriteByte(sectionMask);");
        sb.AppendLine("        if (hasComponents)");
        sb.AppendLine($"            {componentSectionWriter}(ref writer);");
        sb.AppendLine("    }");
    }

    // Client-ctx variant. Emits only ApplyOwnerSnapshot / ApplyOtherSnapshot
    // — symmetric counterparts of the cell-side SerializeForOwner/OtherClient.
    // Base-scope client-visible props (BaseAndClient) stay out: they arrive
    // via ApplyReplicatedDelta on the delta channel, not in the snapshot.
    // Keeping base-scope out preserves lockstep between cell's serializer
    // and client's deserializer.
    private static string? EmitClientSnapshotApplyMethods(EntityDefModel def, string className,
                                                           string namespaceName,
                                                           List<PropertyDefModel> replicableProps,
                                                           bool hasComponents)
    {
        var ownerFields = replicableProps
            .Where(p => IsOwnerVisible(p.Scope) && p.Scope.IsCell()).ToList();
        var otherFields = replicableProps
            .Where(p => IsOtherVisible(p.Scope) && p.Scope.IsCell()).ToList();
        if (ownerFields.Count == 0 && otherFields.Count == 0 && !hasComponents) return null;

        var sb = new StringBuilder();
        EmitterHeader.Write(sb, namespaceName);
        sb.AppendLine($"partial class {className}");
        sb.AppendLine("{");

        if (ownerFields.Count > 0)
        {
            EmitScopeApply(sb, ownerFields, "ApplyOwnerSnapshot");
            sb.AppendLine();
        }
        if (otherFields.Count > 0)
        {
            EmitScopeApply(sb, otherFields, "ApplyOtherSnapshot");
            sb.AppendLine();
        }

        // ApplyReplicatedDelta — decode-time scope-agnostic; the server's
        // audience mask guarantees only visible bits are ever set, so
        // iterating every replicable prop is safe. Changed fields fire
        // OnXxxChanged so scripts observe the transition. Component
        // section (bit 2) dispatches into per-slot ApplyDelta when the
        // entity declares components.
        EmitClientApplyReplicatedDelta(sb, replicableProps, hasComponents);

        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitClientApplyReplicatedDelta(StringBuilder sb,
                                                        List<PropertyDefModel> replicableProps,
                                                        bool hasComponents)
    {
        sb.AppendLine("    public override void ApplyReplicatedDelta(ref SpanReader reader)");
        sb.AppendLine("    {");
        sb.AppendLine("        byte sectionMask = reader.ReadByte();");
        // ReplicatedDirtyFlags only exists when the entity has own
        // replicable props. Components-only entities still see
        // sectionMask but never bits 0/1, so skip the read.
        if (replicableProps.Count > 0)
        {
            var (_, _, _, readerMethod, _) = GetFlagsTypeInfo(replicableProps.Count);
            sb.AppendLine("        if ((sectionMask & 0x01) != 0)");
            sb.AppendLine("        {");
            sb.AppendLine($"            var scalarFlags = (ReplicatedDirtyFlags)reader.{readerMethod}();");
            foreach (var prop in replicableProps)
            {
                if (PropertyCodec.IsList(prop) || PropertyCodec.IsDict(prop)) continue;
                var propName = DefTypeHelper.ToPropertyName(prop.Name);
                var fieldName = DefTypeHelper.ToFieldName(prop.Name);
                sb.AppendLine($"            if ((scalarFlags & ReplicatedDirtyFlags.{propName}) != 0)");
                sb.AppendLine("            {");
                sb.AppendLine($"                var old{propName} = {fieldName};");
                sb.AppendLine($"                {fieldName} = {PropertyCodec.ReadExpr(prop, "reader")};");
                sb.AppendLine($"                On{propName}Changed(old{propName}, {fieldName});");
                sb.AppendLine("            }");
            }
            sb.AppendLine("        }");
            sb.AppendLine("        if ((sectionMask & 0x02) != 0)");
            sb.AppendLine("        {");
            sb.AppendLine($"            var containerFlags = (ReplicatedDirtyFlags)reader.{readerMethod}();");
            foreach (var prop in replicableProps)
            {
                if (!PropertyCodec.IsList(prop) && !PropertyCodec.IsDict(prop)) continue;
                var propName = DefTypeHelper.ToPropertyName(prop.Name);
                sb.AppendLine($"            if ((containerFlags & ReplicatedDirtyFlags.{propName}) != 0)");
                if (PropertyCodec.IsList(prop))
                    PropertyCodec.EmitListOpLogRead(sb, prop, "reader", "            ");
                else
                    PropertyCodec.EmitDictOpLogRead(sb, prop, "reader", "            ");
            }
            sb.AppendLine("        }");
        }
        // Component section (bit 2): per-slot ApplyDelta dispatch lives
        // on the entity's Components.g.cs partial (emitted by
        // EntityComponentAccessorEmitter). For entities without any
        // declared components we still bail safely, but only an
        // out-of-band sender could set bit 2 on those — Advance keeps
        // the reader cursor consistent for any trailing channel framing.
        sb.AppendLine("        if ((sectionMask & 0x04) != 0)");
        sb.AppendLine("        {");
        if (hasComponents)
            sb.AppendLine("            ApplyComponentSection(ref reader);");
        else
            sb.AppendLine("            reader.Advance(reader.Remaining);");
        sb.AppendLine("        }");
        sb.AppendLine("    }");
    }

    private static void EmitScopeApply(StringBuilder sb, List<PropertyDefModel> props,
                                       string methodName)
    {
        // Direct-to-backing-field writes: a snapshot is an authoritative
        // state reset, not an observed change, so property-change callbacks
        // (OnXxxChanged, emitted by PropertiesEmitter) MUST NOT fire here.
        // Setter callbacks on the delta path live in a separate method.
        sb.AppendLine($"    public override void {methodName}(ref SpanReader reader)");
        sb.AppendLine("    {");
        foreach (var prop in props)
        {
            if (PropertyCodec.IsList(prop))
            {
                PropertyCodec.EmitListRead(sb, prop, "reader", "        ");
                continue;
            }
            if (PropertyCodec.IsDict(prop))
            {
                PropertyCodec.EmitDictRead(sb, prop, "reader", "        ");
                continue;
            }
            var fieldName = DefTypeHelper.ToFieldName(prop.Name);
            sb.AppendLine($"        {fieldName} = {PropertyCodec.ReadExpr(prop, "reader")};");
        }
        sb.AppendLine("    }");
    }

    private static void EmitAudienceMasks(StringBuilder sb, List<PropertyDefModel> props)
    {
        string MaskExpr(System.Func<PropertyDefModel, bool> pred)
        {
            var names = props.Where(pred)
                             .Select(p => "ReplicatedDirtyFlags." + DefTypeHelper.ToPropertyName(p.Name))
                             .ToList();
            return names.Count == 0 ? "ReplicatedDirtyFlags.None" : string.Join(" | ", names);
        }

        bool IsScalar(PropertyDefModel p) => !PropertyCodec.IsList(p) && !PropertyCodec.IsDict(p);
        bool IsContainer(PropertyDefModel p) => PropertyCodec.IsList(p) || PropertyCodec.IsDict(p);

        // Split per audience × kind so the sectionMask wire encoding can
        // emit / decode each section independently without re-scanning
        // every dirty bit.
        sb.AppendLine($"    private const ReplicatedDirtyFlags OwnerVisibleScalarMask = {MaskExpr(p => IsOwnerVisible(p.Scope) && IsScalar(p))};");
        sb.AppendLine($"    private const ReplicatedDirtyFlags OwnerVisibleContainerMask = {MaskExpr(p => IsOwnerVisible(p.Scope) && IsContainer(p))};");
        sb.AppendLine($"    private const ReplicatedDirtyFlags OtherVisibleScalarMask = {MaskExpr(p => IsOtherVisible(p.Scope) && IsScalar(p))};");
        sb.AppendLine($"    private const ReplicatedDirtyFlags OtherVisibleContainerMask = {MaskExpr(p => IsOtherVisible(p.Scope) && IsContainer(p))};");
        sb.AppendLine($"    private const ReplicatedDirtyFlags OwnerVisibleMask = OwnerVisibleScalarMask | OwnerVisibleContainerMask;");
        sb.AppendLine($"    private const ReplicatedDirtyFlags OtherVisibleMask = OtherVisibleScalarMask | OtherVisibleContainerMask;");
    }

    private static void EmitAudienceDeltaSerializer(StringBuilder sb, List<PropertyDefModel> props,
                                                    string methodName, string scalarMaskName,
                                                    string containerMaskName,
                                                    System.Func<PropertyDefModel, bool> audiencePred,
                                                    bool hasComponents,
                                                    string componentDirtyPredicate,
                                                    string componentSectionWriter)
    {
        var (_, _, writerMethod, _, castPrefix) = GetFlagsTypeInfo(props.Count);
        sb.AppendLine($"    public void {methodName}(ref SpanWriter writer)");
        sb.AppendLine("    {");
        sb.AppendLine($"        var scalarFlags = _dirtyFlags & {scalarMaskName};");
        sb.AppendLine($"        var containerFlags = _dirtyFlags & {containerMaskName};");
        if (hasComponents)
            sb.AppendLine($"        bool hasComponents = {componentDirtyPredicate}();");
        sb.AppendLine("        byte sectionMask = 0;");
        sb.AppendLine("        if (scalarFlags != ReplicatedDirtyFlags.None) sectionMask |= 0x01;");
        sb.AppendLine("        if (containerFlags != ReplicatedDirtyFlags.None) sectionMask |= 0x02;");
        if (hasComponents)
            sb.AppendLine("        if (hasComponents) sectionMask |= 0x04;");
        sb.AppendLine("        writer.WriteByte(sectionMask);");
        sb.AppendLine();
        sb.AppendLine("        if (scalarFlags != ReplicatedDirtyFlags.None)");
        sb.AppendLine("        {");
        sb.AppendLine($"            writer.{writerMethod}({castPrefix}scalarFlags);");
        foreach (var prop in props)
        {
            if (!audiencePred(prop)) continue;
            if (PropertyCodec.IsList(prop) || PropertyCodec.IsDict(prop)) continue;
            var propName = DefTypeHelper.ToPropertyName(prop.Name);
            var fieldName = DefTypeHelper.ToFieldName(prop.Name);
            sb.AppendLine($"            if ((scalarFlags & ReplicatedDirtyFlags.{propName}) != 0)");
            sb.AppendLine($"                {PropertyCodec.WriteExpr(prop, "writer", fieldName)};");
        }
        sb.AppendLine("        }");
        sb.AppendLine();
        sb.AppendLine("        if (containerFlags != ReplicatedDirtyFlags.None)");
        sb.AppendLine("        {");
        sb.AppendLine($"            writer.{writerMethod}({castPrefix}containerFlags);");
        foreach (var prop in props)
        {
            if (!audiencePred(prop)) continue;
            if (!PropertyCodec.IsList(prop) && !PropertyCodec.IsDict(prop)) continue;
            var propName = DefTypeHelper.ToPropertyName(prop.Name);
            sb.AppendLine($"            if ((containerFlags & ReplicatedDirtyFlags.{propName}) != 0)");
            sb.AppendLine("            {");
            if (PropertyCodec.IsList(prop))
                PropertyCodec.EmitListOpLogWrite(sb, prop, "writer", "                ");
            else
                PropertyCodec.EmitDictOpLogWrite(sb, prop, "writer", "                ");
            sb.AppendLine("            }");
        }
        sb.AppendLine("        }");
        if (hasComponents)
        {
            sb.AppendLine();
            sb.AppendLine("        if (hasComponents)");
            sb.AppendLine($"            {componentSectionWriter}(ref writer);");
        }
        sb.AppendLine("    }");
    }

    private static void EmitBuildAndConsume(StringBuilder sb, bool hasOwnerSnapshot,
                                            bool hasOtherSnapshot, bool hasComponents,
                                            bool hasOwnProps)
    {
        // Zero-alloc API: caller owns the four SpanWriters (pool-rented,
        // Reset() between entities). Returns (eventSeq, volatileSeq) via
        // out params; avoids per-tick byte[] churn on high-fanout pumps.
        sb.AppendLine("    public override bool BuildAndConsumeReplicationFrame(");
        sb.AppendLine("        ref SpanWriter ownerSnapshot, ref SpanWriter otherSnapshot,");
        sb.AppendLine("        ref SpanWriter ownerDelta, ref SpanWriter otherDelta,");
        sb.AppendLine("        out ulong eventSeq, out ulong volatileSeq)");
        sb.AppendLine("    {");
        // hasEvent gate folds in components when present; for components-
        // only entities, _dirtyFlags / OwnerVisibleMask don't exist so we
        // can't reference them. The component check goes through the
        // audience predicates rather than the raw _dirtyComponents bitmap
        // so AddComponent / RemoveComponent (which flip the bit but don't
        // mark any property dirty) don't emit phantom empty frames.
        // ClearDirtyComponents resets the bitmap after the dual write.
        if (hasOwnProps && hasComponents)
            sb.AppendLine("        bool hasEvent = (_dirtyFlags & (OwnerVisibleMask | OtherVisibleMask)) != ReplicatedDirtyFlags.None || HasOwnerDirtyComponent() || HasOtherDirtyComponent();");
        else if (hasOwnProps)
            sb.AppendLine("        bool hasEvent = (_dirtyFlags & (OwnerVisibleMask | OtherVisibleMask)) != ReplicatedDirtyFlags.None;");
        else  // components-only
            sb.AppendLine("        bool hasEvent = HasOwnerDirtyComponent() || HasOtherDirtyComponent();");
        sb.AppendLine("        bool hasVolatile = VolatileDirtyCore;");
        sb.AppendLine("        if (!hasEvent && !hasVolatile)");
        sb.AppendLine("        {");
        sb.AppendLine("            eventSeq = 0; volatileSeq = 0;");
        sb.AppendLine("            return false;");
        sb.AppendLine("        }");
        sb.AppendLine();
        sb.AppendLine("        if (hasEvent)");
        sb.AppendLine("        {");
        sb.AppendLine("            _eventSeq++;");
        if (hasOwnerSnapshot)
            sb.AppendLine("            SerializeForOwnerClient(ref ownerSnapshot);");
        if (hasOtherSnapshot)
            sb.AppendLine("            SerializeForOtherClients(ref otherSnapshot);");
        sb.AppendLine("            SerializeOwnerDelta(ref ownerDelta);");
        sb.AppendLine("            SerializeOtherDelta(ref otherDelta);");
        if (hasOwnProps)
            sb.AppendLine("            _dirtyFlags = ReplicatedDirtyFlags.None;");
        if (hasComponents)
            sb.AppendLine("            ClearDirtyComponents();");
        sb.AppendLine("        }");
        sb.AppendLine();
        sb.AppendLine("        if (hasVolatile)");
        sb.AppendLine("        {");
        sb.AppendLine("            _volatileSeq++;");
        sb.AppendLine("            VolatileDirtyCore = false;");
        sb.AppendLine("        }");
        sb.AppendLine();
        sb.AppendLine("        eventSeq = hasEvent ? _eventSeq : 0UL;");
        sb.AppendLine("        volatileSeq = hasVolatile ? _volatileSeq : 0UL;");
        sb.AppendLine("        return true;");
        sb.AppendLine("    }");
    }

    private static void EmitApplyReplicatedDelta(StringBuilder sb, List<PropertyDefModel> props)
    {
        var (_, _, _, readerMethod, _) = GetFlagsTypeInfo(props.Count);
        sb.AppendLine("    public void ApplyReplicatedDelta(ref SpanReader reader)");
        sb.AppendLine("    {");
        sb.AppendLine("        byte sectionMask = reader.ReadByte();");
        sb.AppendLine("        if ((sectionMask & 0x01) != 0)");
        sb.AppendLine("        {");
        sb.AppendLine($"            var scalarFlags = (ReplicatedDirtyFlags)reader.{readerMethod}();");
        foreach (var prop in props)
        {
            if (PropertyCodec.IsList(prop) || PropertyCodec.IsDict(prop)) continue;
            var propName = DefTypeHelper.ToPropertyName(prop.Name);
            var fieldName = DefTypeHelper.ToFieldName(prop.Name);
            sb.AppendLine($"            if ((scalarFlags & ReplicatedDirtyFlags.{propName}) != 0)");
            sb.AppendLine($"                {fieldName} = {PropertyCodec.ReadExpr(prop, "reader")};");
        }
        sb.AppendLine("        }");
        sb.AppendLine("        if ((sectionMask & 0x02) != 0)");
        sb.AppendLine("        {");
        sb.AppendLine($"            var containerFlags = (ReplicatedDirtyFlags)reader.{readerMethod}();");
        foreach (var prop in props)
        {
            if (!PropertyCodec.IsList(prop) && !PropertyCodec.IsDict(prop)) continue;
            var propName = DefTypeHelper.ToPropertyName(prop.Name);
            sb.AppendLine($"            if ((containerFlags & ReplicatedDirtyFlags.{propName}) != 0)");
            if (PropertyCodec.IsList(prop))
                PropertyCodec.EmitListOpLogRead(sb, prop, "reader", "            ");
            else
                PropertyCodec.EmitDictOpLogRead(sb, prop, "reader", "            ");
        }
        sb.AppendLine("        }");
        sb.AppendLine("    }");
    }

    private static void EmitScopeSerialize(StringBuilder sb, List<PropertyDefModel> props,
                                            string methodName)
    {
        // SerializeForOwnerClient / SerializeForOtherClients are virtual on ServerEntity
        // so the baseline pump can invoke them polymorphically on any entity.
        sb.AppendLine($"    public override void {methodName}(ref SpanWriter writer)");
        sb.AppendLine("    {");
        foreach (var prop in props)
        {
            if (PropertyCodec.IsList(prop))
            {
                PropertyCodec.EmitListWrite(sb, prop, "writer", "        ");
                continue;
            }
            if (PropertyCodec.IsDict(prop))
            {
                PropertyCodec.EmitDictWrite(sb, prop, "writer", "        ");
                continue;
            }
            var fieldName = DefTypeHelper.ToFieldName(prop.Name);
            sb.AppendLine($"        {PropertyCodec.WriteExpr(prop, "writer", fieldName)};");
        }
        sb.AppendLine("    }");
    }

    // Delegate to the shared PropertyScopeExtensions helpers so every caller
    // (emitters, DEF008 detection, runtime) resolves scope membership
    // against the same rulebook.
    private static bool IsOwnerVisible(PropertyScope scope) => scope.IsOwnClientVisible();
    private static bool IsOtherVisible(PropertyScope scope) => scope.IsOtherClientsVisible();

    // Decide whether a replicable scope belongs on this process's generated
    // class. Client always sees every replicable property (from its
    // observer-side projection); Server (legacy fallback) also sees
    // everything; Base owns BaseAndClient; Cell owns the rest.
    private static bool IsOnThisSide(PropertyScope scope, ProcessContext ctx) => ctx switch
    {
        ProcessContext.Base => scope.IsBase(),
        ProcessContext.Cell => scope.IsCell(),
        _ => true,  // Client, Server — emit everything the caller has already filtered
    };

    private static (string BackingType, string FlagSuffix, string WriterMethod, string ReaderMethod, string CastPrefix) GetFlagsTypeInfo(int fieldCount)
    {
        if (fieldCount <= 8) return ("byte", "u", "WriteByte", "ReadByte", "(byte)");
        if (fieldCount <= 16) return ("ushort", "u", "WriteUInt16", "ReadUInt16", "(ushort)");
        if (fieldCount <= 32) return ("uint", "u", "WriteUInt32", "ReadUInt32", "(uint)");
        return ("ulong", "UL", "WriteUInt64", "ReadUInt64", "(ulong)");
    }
}
