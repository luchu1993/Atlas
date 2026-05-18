using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Atlas.Generators.Def;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;

namespace Atlas.Tools.CppEmitter;

public static class Program
{
    public static int Main(string[] args)
    {
        string? entityDefsDir = null;
        string? outputDir = null;
        string @namespace = "atlas::mvp";

        for (int i = 0; i < args.Length; ++i)
        {
            var arg = args[i];
            switch (arg)
            {
                case "--entity-defs":
                {
                    if (!TryReadValue(args, ref i, arg, out var v)) return 1;
                    entityDefsDir = v; break;
                }
                case "--output":
                {
                    if (!TryReadValue(args, ref i, arg, out var v)) return 1;
                    outputDir = v; break;
                }
                case "--namespace":
                {
                    if (!TryReadValue(args, ref i, arg, out var v)) return 1;
                    @namespace = v; break;
                }
                case "-h":
                case "--help":
                    PrintUsage(); return 0;
                default:
                    Console.Error.WriteLine($"CppEmitter: unknown argument '{arg}'");
                    PrintUsage();
                    return 1;
            }
        }

        if (entityDefsDir is null || outputDir is null)
        {
            PrintUsage();
            return 1;
        }

        var entityDefsPath = Path.GetFullPath(entityDefsDir);
        var outputPath = Path.GetFullPath(outputDir);

        if (!Directory.Exists(entityDefsPath))
        {
            Console.Error.WriteLine($"CppEmitter: --entity-defs '{entityDefsPath}' is not a directory");
            return 2;
        }
        Directory.CreateDirectory(outputPath);

        var manifestPath = Path.Combine(entityDefsPath, EntityIdManifestParser.FileName);
        if (!File.Exists(manifestPath))
        {
            Console.Error.WriteLine($"CppEmitter: missing {EntityIdManifestParser.FileName} in {entityDefsPath}");
            return 3;
        }

        var diagnostics = new List<Diagnostic>();
        void Report(Diagnostic d) => diagnostics.Add(d);

        var manifest = EntityIdManifestParser.Parse(File.ReadAllText(manifestPath), manifestPath, Report);
        if (manifest is null)
        {
            FlushDiagnostics(diagnostics);
            Console.Error.WriteLine("CppEmitter: failed to parse entity_ids.xml");
            return 4;
        }

        ComponentIdManifest? componentManifest = null;
        var componentManifestPath = Path.Combine(entityDefsPath, ComponentIdManifestParser.FileName);
        if (File.Exists(componentManifestPath))
        {
            componentManifest = ComponentIdManifestParser.Parse(
                File.ReadAllText(componentManifestPath), componentManifestPath, Report);
        }

        var entities = new List<EntityDefModel>();
        var standaloneComponents = new List<ComponentDefModel>();
        foreach (var defPath in Directory.EnumerateFiles(entityDefsPath, "*.def"))
        {
            var text = SourceText.From(File.ReadAllText(defPath));
            var parsed = DefParser.ParseAny(text, defPath, Report);
            if (parsed?.Entity is not null) entities.Add(parsed.Entity);
            if (parsed?.StandaloneComponent is not null) standaloneComponents.Add(parsed.StandaloneComponent);
        }

        var linked = DefLinker.Link(entities, standaloneComponents, componentManifest, Report);
        FlushDiagnostics(diagnostics);
        if (linked is null)
        {
            Console.Error.WriteLine("CppEmitter: DefLinker failed");
            return 5;
        }

        // Shared struct file (entity-local <types> all linker-merged here).
        // Entity + component gen headers #include it, so cross-entity struct
        // refs (e.g., StressLoadComponent.def references StressWeapon defined
        // in StressAvatar.def) resolve without circular includes.
        EmitSharedStructs(outputPath, @namespace, linked.Structs);

        // Components first so entity gen files can #include them.
        int compsEmitted = 0;
        int compsSkipped = 0;
        foreach (var comp in linked.StandaloneComponents)
        {
            if (comp.Locality != ComponentLocality.Synced)
            {
                ++compsSkipped;
                continue;
            }
            try
            {
                EmitComponent(outputPath, @namespace, comp);
                ++compsEmitted;
            }
            catch (UnsupportedFeatureException ex)
            {
                Console.Error.WriteLine(
                    $"CppEmitter: component '{comp.TypeName}' uses feature not yet emitted — skipping ({ex.Message})");
                ++compsSkipped;
            }
        }

        int emitted = 0;
        int skipped = 0;
        foreach (var entity in linked.Entities)
        {
            if (!manifest.ActiveByName.TryGetValue(entity.Name, out var typeId))
            {
                Console.Error.WriteLine($"CppEmitter: entity '{entity.Name}' has no active id in entity_ids.xml — skipping");
                ++skipped;
                continue;
            }
            try
            {
                EmitEntity(outputPath, @namespace, entity, typeId);
                ++emitted;
            }
            catch (UnsupportedFeatureException ex)
            {
                Console.Error.WriteLine(
                    $"CppEmitter: entity '{entity.Name}' uses feature not yet emitted — skipping ({ex.Message})");
                ++skipped;
            }
        }

        Console.WriteLine($"CppEmitter: emitted {emitted}/{linked.Entities.Count} entity headers + {compsEmitted} component headers ({skipped + compsSkipped} skipped) to {outputPath}");
        return 0;
    }

    private sealed class UnsupportedFeatureException : Exception
    {
        public UnsupportedFeatureException(string message) : base(message) { }
    }

    private static void EmitEntity(string outputDir, string @namespace, EntityDefModel entity,
                                    ushort typeId)
    {
        var className = entity.Name;
        var headerName = $"{entity.Name}.gen.h";
        var guard = $"ATLAS_GEN_{entity.Name.ToUpperInvariant()}_GEN_H_";

        var syncedComps = entity.Components
            .Where(c => c.Locality == ComponentLocality.Synced && c.SlotIdx >= 0)
            .ToList();

        var sb = new StringBuilder();
        sb.AppendLine("// Auto-generated by Atlas.Tools.CppEmitter — do not edit.");
        sb.AppendLine($"#ifndef {guard}");
        sb.AppendLine($"#define {guard}");
        sb.AppendLine();
        sb.AppendLine("#include <cstdint>");
        sb.AppendLine("#include <memory>");
        sb.AppendLine("#include <string>");
        sb.AppendLine("#include <vector>");
        sb.AppendLine();
        sb.AppendLine("#include \"AtlasCore/client_entity.h\"");
        sb.AppendLine("#include \"AtlasCore/component_instance.h\"");
        sb.AppendLine("#include \"AtlasCore/rpc_sender.h\"");
        sb.AppendLine("#include \"AtlasCore/span_reader.h\"");
        sb.AppendLine("#include \"AtlasCore/span_writer.h\"");
        sb.AppendLine();
        sb.AppendLine("#include \"gen/_Structs.gen.h\"");
        foreach (var c in syncedComps)
        {
            sb.AppendLine($"#include \"gen/{c.TypeName}.gen.h\"");
        }
        sb.AppendLine();
        sb.AppendLine($"namespace {@namespace} {{");
        sb.AppendLine();

        sb.AppendLine($"class {className} : public atlas::ClientEntity {{");
        sb.AppendLine(" public:");
        sb.AppendLine($"  static constexpr atlas::EntityTypeId kTypeId = {typeId};");
        sb.AppendLine();
        sb.AppendLine($"  {className}(atlas::EntityId id, atlas::EntityTypeId type) :");
        if (syncedComps.Count == 0)
        {
            sb.AppendLine("      atlas::ClientEntity(id, type) {}");
        }
        else
        {
            sb.AppendLine("      atlas::ClientEntity(id, type) {");
            foreach (var c in syncedComps)
            {
                sb.AppendLine($"    RegisterComponentFactory({c.SlotIdx},");
                sb.AppendLine($"        [](const AtlasEdrComponent* __d, atlas::ClientEntity* __o, uint8_t __s) {{");
                sb.AppendLine($"          return std::make_unique<{@namespace}::{c.TypeName}>(__d, __o, __s);");
                sb.AppendLine($"        }});");
            }
            sb.AppendLine("  }");
        }
        sb.AppendLine();
        if (syncedComps.Count > 0)
        {
            foreach (var c in syncedComps)
            {
                sb.AppendLine($"  {@namespace}::{c.TypeName}* {c.SlotName}() const {{");
                sb.AppendLine($"    return static_cast<{@namespace}::{c.TypeName}*>(GetComponent({c.SlotIdx}));");
                sb.AppendLine("  }");
            }
            sb.AppendLine();
        }

        EmitPropertyGetters(sb, entity);
        EmitContainerAndStructGetters(sb, entity, @namespace);
        EmitPropertyChangeHooks(sb, entity);
        EmitApplyDeltaOverride(sb, entity);
        EmitRpcConstants(sb, entity, typeId);
        EmitUpstreamRpcStubs(sb, entity, typeId, @namespace);
        EmitDownstreamRpcHandlers(sb, entity, @namespace);
        EmitDispatchRpcOverride(sb, entity, @namespace);

        sb.AppendLine("};");
        sb.AppendLine();
        sb.AppendLine($"}}  // namespace {@namespace}");
        sb.AppendLine();
        sb.AppendLine($"#endif  // {guard}");

        File.WriteAllText(Path.Combine(outputDir, headerName), sb.ToString());
    }

    private readonly record struct ScalarPropEntry(int Index, string PropName, string CppType,
                                                    bool ReturnsByRef);

    private static IEnumerable<ScalarPropEntry> EnumerateClientVisibleScalars(EntityDefModel entity)
    {
        // Descriptor index follows .def declaration order with reserved-position
        // properties excluded (matches TypeRegistryEmitter's effectiveProps).
        // Containers / structs land in the second pass once codegen tracks
        // recursive value access; for now plain scalars cover MVP entities.
        var effective = entity.Properties.Where(p => !p.IsReservedPosition).ToList();
        for (int i = 0; i < effective.Count; ++i)
        {
            var prop = effective[i];
            if (!prop.Scope.IsClientVisible()) continue;
            if (prop.TypeRef is not null) continue;
            var cpp = MapDefTypeToCpp(prop.Type);
            if (cpp is null) continue;
            yield return new ScalarPropEntry(
                Index: i,
                PropName: DefTypeHelper.ToPropertyName(prop.Name),
                CppType: cpp,
                ReturnsByRef: cpp == "std::string" || cpp == "std::vector<uint8_t>");
        }
    }

    private static void EmitPropertyGetters(StringBuilder sb, EntityDefModel entity)
    {
        bool any = false;
        foreach (var p in EnumerateClientVisibleScalars(entity))
        {
            if (p.ReturnsByRef)
            {
                sb.AppendLine($"  const {p.CppType}& {p.PropName}() const {{");
                sb.AppendLine($"    if (const {p.CppType}* v = GetScalar<{p.CppType}>({p.Index})) return *v;");
                sb.AppendLine($"    static const {p.CppType} kEmpty{{}};");
                sb.AppendLine($"    return kEmpty;");
                sb.AppendLine("  }");
            }
            else
            {
                sb.AppendLine($"  {p.CppType} {p.PropName}() const {{");
                sb.AppendLine($"    if (const {p.CppType}* v = GetScalar<{p.CppType}>({p.Index})) return *v;");
                sb.AppendLine($"    return {p.CppType}{{}};");
                sb.AppendLine("  }");
            }
            any = true;
        }
        if (any) sb.AppendLine();
    }

    // Per scalar prop: a virtual that game code overrides to react to a value
    // change. Fires only when the new wire value differs from the prior local
    // state (avoids spurious calls if a sender re-sends the same value).
    private static void EmitPropertyChangeHooks(StringBuilder sb, EntityDefModel entity)
    {
        bool any = false;
        foreach (var p in EnumerateClientVisibleScalars(entity))
        {
            var argType = p.ReturnsByRef ? $"const {p.CppType}&" : p.CppType;
            sb.AppendLine($"  virtual void On{p.PropName}Changed({argType} /*oldValue*/, {argType} /*newValue*/) {{}}");
            any = true;
        }
        if (any) sb.AppendLine();
    }

    // Snapshot → base ApplyDelta → diff → fire. Captures old values up-front
    // so the base call can overwrite the variant in-place. Skipped if the
    // entity has no scalar hooks (no work to do beyond base).
    private static void EmitApplyDeltaOverride(StringBuilder sb, EntityDefModel entity)
    {
        var scalars = EnumerateClientVisibleScalars(entity).ToList();
        if (scalars.Count == 0) return;
        sb.AppendLine("  bool ApplyDelta(atlas::SpanReader& reader) override {");
        foreach (var p in scalars)
        {
            sb.AppendLine($"    auto old_{p.PropName} = {p.PropName}();");
        }
        sb.AppendLine("    if (!atlas::ClientEntity::ApplyDelta(reader)) return false;");
        foreach (var p in scalars)
        {
            sb.AppendLine($"    if (auto new_{p.PropName} = {p.PropName}();");
            sb.AppendLine($"        new_{p.PropName} != old_{p.PropName}) On{p.PropName}Changed(old_{p.PropName}, new_{p.PropName});");
        }
        sb.AppendLine("    return true;");
        sb.AppendLine("  }");
        sb.AppendLine();
    }

    private static void EmitRpcConstants(StringBuilder sb, EntityDefModel entity, ushort typeId)
    {
        bool any = false;
        EmitRpcConstantSection(sb, entity.ClientMethods, typeId, RpcDirection.Client, ref any);
        EmitRpcConstantSection(sb, entity.CellMethods, typeId, RpcDirection.Cell, ref any);
        EmitRpcConstantSection(sb, entity.BaseMethods, typeId, RpcDirection.Base, ref any);
        if (any) sb.AppendLine();
    }

    private static void EmitRpcConstantSection(StringBuilder sb, List<MethodDefModel> methods,
                                                ushort typeId, RpcDirection dir, ref bool any)
    {
        // RPC ID index uses .def declaration position regardless of whether
        // codegen can map the args — kRpcId_<Name> stays stable so server
        // and client agree on the wire even when codegen skips the stub.
        for (int idx = 0; idx < methods.Count; ++idx)
        {
            var m = methods[idx];
            if (!MethodArgsSupported(m)) continue;
            var rpcId = EncodeRpcId(slot: 0, direction: (byte)dir, typeIndex: typeId,
                                     methodIdx: idx + 1);
            sb.AppendLine($"  static constexpr uint32_t kRpcId_{m.Name} = 0x{rpcId:X8}u;");
            any = true;
        }
    }

    private static bool MethodArgsSupported(MethodDefModel m)
    {
        foreach (var a in m.Args)
        {
            if (!ArgTypeSupported(a)) return false;
        }
        return true;
    }

    private static bool ArgTypeSupported(ArgDefModel arg)
    {
        if (arg.TypeRef is not null) return IsTypeRefSupported(arg.TypeRef);
        return MapDefTypeToCpp(arg.Type) is not null;
    }

    private static bool IsTypeRefSupported(DataTypeRefModel r)
    {
        return r.Kind switch
        {
            PropertyDataKind.Struct => r.StructName is not null,
            PropertyDataKind.List => r.Elem is not null && IsTypeRefSupported(r.Elem),
            PropertyDataKind.Dict => r.Key is not null && r.Elem is not null
                                     && IsTypeRefSupported(r.Key) && IsTypeRefSupported(r.Elem),
            PropertyDataKind.Custom => false,
            _ => MapScalarKindToCpp(r.Kind) is not null,
        };
    }

    // rpc_id wire layout (mirrors Atlas.Generators.Def.Emitters.RpcIdEncoder):
    //   bits 24-30  slot_idx (0 = entity body)
    //   bits 22-23  direction (0=Client, 2=Cell, 3=Base)
    //   bits  8-21  typeIndex
    //   bits  0-7   methodIdx (1-based per direction)
    private static uint EncodeRpcId(int slot, byte direction, ushort typeIndex, int methodIdx)
    {
        return ((uint)slot << 24) | ((uint)direction << 22) | ((uint)typeIndex << 8) | (uint)methodIdx;
    }

    private static void EmitUpstreamRpcStubs(StringBuilder sb, EntityDefModel entity, ushort typeId,
                                              string @namespace)
    {
        EmitUpstreamSection(sb, entity.CellMethods, "SendCellRpc", @namespace);
        EmitUpstreamSection(sb, entity.BaseMethods, "SendBaseRpc", @namespace);
    }

    private static void EmitUpstreamSection(StringBuilder sb, List<MethodDefModel> methods,
                                             string senderMethod, string @namespace)
    {
        foreach (var m in methods)
        {
            if (!MethodArgsSupported(m)) continue;
            var argList = string.Join(", ",
                m.Args.Select(a => $"{ArgTypeForSignature(a, @namespace)} {a.Name}"));
            sb.AppendLine($"  void {m.Name}({argList}) {{");
            sb.AppendLine("    atlas::RpcSender* __sender = Sender();");
            sb.AppendLine("    if (__sender == nullptr) return;");
            sb.AppendLine("    atlas::SpanWriter __writer;");
            foreach (var a in m.Args)
            {
                var ref_ = TypeRefForArg(a);
                var names = new FreshNames();
                EmitWriteValue(sb, "    ", ref_, "__writer", a.Name, @namespace, names);
            }
            sb.AppendLine($"    __sender->{senderMethod}(Id(), kRpcId_{m.Name},");
            sb.AppendLine("        __writer.Bytes().data(), static_cast<int32_t>(__writer.Size()));");
            sb.AppendLine("  }");
            sb.AppendLine();
        }
    }

    // Promotes a scalar arg's string type to a synthetic DataTypeRefModel so
    // EmitWriteValue / EmitReadValue can drive both scalar and container args
    // through one recursive path.
    private static DataTypeRefModel TypeRefForArg(ArgDefModel arg)
    {
        if (arg.TypeRef is not null) return arg.TypeRef;
        return new DataTypeRefModel { Kind = KindOfDefType(arg.Type) };
    }

    private static PropertyDataKind KindOfDefType(string defType) => defType.ToLowerInvariant() switch
    {
        "bool" => PropertyDataKind.Bool,
        "int8" => PropertyDataKind.Int8,
        "uint8" => PropertyDataKind.UInt8,
        "int16" => PropertyDataKind.Int16,
        "uint16" => PropertyDataKind.UInt16,
        "int32" => PropertyDataKind.Int32,
        "uint32" => PropertyDataKind.UInt32,
        "int64" => PropertyDataKind.Int64,
        "uint64" => PropertyDataKind.UInt64,
        "float" => PropertyDataKind.Float,
        "double" => PropertyDataKind.Double,
        "string" => PropertyDataKind.String,
        "bytes" => PropertyDataKind.Bytes,
        "vector3" => PropertyDataKind.Vector3,
        "quaternion" => PropertyDataKind.Quaternion,
        _ => PropertyDataKind.Custom,
    };

    // Each client_method gets a virtual stub (empty body so game code is free
    // to override only what it cares about) plus a switch arm in DispatchRpc.
    // Param names commented out in the base sig to avoid -Wunused-parameter.
    private static void EmitDownstreamRpcHandlers(StringBuilder sb, EntityDefModel entity,
                                                   string @namespace)
    {
        var supported = entity.ClientMethods.Where(MethodArgsSupported).ToList();
        if (supported.Count == 0) return;
        foreach (var m in supported)
        {
            var argSig = string.Join(", ",
                m.Args.Select(a => $"{ArgTypeForSignature(a, @namespace)} /*{a.Name}*/"));
            sb.AppendLine($"  virtual void {m.Name}({argSig}) {{}}");
        }
        sb.AppendLine();
    }

    private static void EmitDispatchRpcOverride(StringBuilder sb, EntityDefModel entity,
                                                 string @namespace)
    {
        var supported = entity.ClientMethods.Where(MethodArgsSupported).ToList();
        if (supported.Count == 0) return;
        sb.AppendLine("  bool DispatchEntityRpc(uint32_t __rpc_id, uint64_t /*trace_id*/,");
        sb.AppendLine("                          atlas::SpanReader& __reader) override {");
        sb.AppendLine("    switch (__rpc_id) {");
        foreach (var m in supported)
        {
            sb.AppendLine($"      case kRpcId_{m.Name}: {{");
            foreach (var a in m.Args)
            {
                var cpp = CppTypeForArg(a, @namespace);
                sb.AppendLine($"        {cpp} {a.Name}{{}};");
                var names = new FreshNames();
                EmitReadValue(sb, "        ", TypeRefForArg(a), "__reader", a.Name, @namespace, names);
            }
            var callArgs = string.Join(", ", m.Args.Select(a => a.Name));
            sb.AppendLine($"        {m.Name}({callArgs});");
            sb.AppendLine("        return true;");
            sb.AppendLine("      }");
        }
        sb.AppendLine("      default: return false;");
        sb.AppendLine("    }");
        sb.AppendLine("  }");
        sb.AppendLine();
    }

    // Bare C++ type for an arg (used as the local-variable type in dispatch).
    private static string CppTypeForArg(ArgDefModel arg, string @namespace)
    {
        if (arg.TypeRef is not null) return CppTypeForRef(arg.TypeRef, @namespace);
        return MapDefTypeToCpp(arg.Type)!;
    }

    // Param signature type for an arg (used in method declarations). Containers,
    // strings, structs go by const-ref; primitives by value.
    private static string ArgTypeForSignature(ArgDefModel arg, string @namespace)
    {
        var cpp = CppTypeForArg(arg, @namespace);
        if (arg.TypeRef is not null) return $"const {cpp}&";
        return cpp switch
        {
            "std::string" => "const std::string&",
            "std::vector<uint8_t>" => "const std::vector<uint8_t>&",
            "atlas::Vec3" => "const atlas::Vec3&",
            "atlas::Quat" => "const atlas::Quat&",
            _ => cpp,
        };
    }

    private enum RpcDirection : byte { Client = 0, Cell = 2, Base = 3 }

    // ── Component class emit ──────────────────────────────────────────────

    // Emits one <ComponentName>.gen.h per synced standalone component.
    // Layout mirrors the entity body: kMethodIdx_* constants, typed scalar
    // getters, container/struct getters, OnXxxChanged hooks, upstream RPC
    // stubs (Owner-routed), downstream virtuals + DispatchRpc switch on
    // method_idx only (RPC direction always 0 = Client at receive time).
    private static void EmitComponent(string outputDir, string @namespace, ComponentDefModel comp)
    {
        var headerName = $"{comp.TypeName}.gen.h";
        var guard = $"ATLAS_GEN_{comp.TypeName.ToUpperInvariant()}_GEN_H_";

        var sb = new StringBuilder();
        sb.AppendLine("// Auto-generated by Atlas.Tools.CppEmitter — do not edit.");
        sb.AppendLine($"#ifndef {guard}");
        sb.AppendLine($"#define {guard}");
        sb.AppendLine();
        sb.AppendLine("#include <cstdint>");
        sb.AppendLine("#include <string>");
        sb.AppendLine("#include <vector>");
        sb.AppendLine();
        sb.AppendLine("#include \"AtlasCore/client_entity.h\"");
        sb.AppendLine("#include \"AtlasCore/component_instance.h\"");
        sb.AppendLine("#include \"AtlasCore/rpc_sender.h\"");
        sb.AppendLine("#include \"AtlasCore/span_reader.h\"");
        sb.AppendLine("#include \"AtlasCore/span_writer.h\"");
        sb.AppendLine();
        sb.AppendLine("#include \"gen/_Structs.gen.h\"");
        sb.AppendLine();
        sb.AppendLine($"namespace {@namespace} {{");
        sb.AppendLine();
        sb.AppendLine($"class {comp.TypeName} : public atlas::ComponentInstance {{");
        sb.AppendLine(" public:");
        sb.AppendLine($"  {comp.TypeName}(const AtlasEdrComponent* desc, atlas::ClientEntity* owner,");
        sb.AppendLine("                       uint8_t slot) :");
        sb.AppendLine("      atlas::ComponentInstance(desc, owner, slot) {}");
        sb.AppendLine();

        EmitComponentPropertyGetters(sb, comp);
        EmitComponentMethodIdxConstants(sb, comp);
        EmitComponentUpstreamStubs(sb, comp, "SendCellRpc", comp.CellMethods, /*dir*/2, @namespace);
        EmitComponentUpstreamStubs(sb, comp, "SendBaseRpc", comp.BaseMethods, /*dir*/3, @namespace);
        EmitComponentDownstreamVirtuals(sb, comp, @namespace);
        EmitComponentDispatchRpc(sb, comp, @namespace);

        sb.AppendLine("};");
        sb.AppendLine();
        sb.AppendLine($"}}  // namespace {@namespace}");
        sb.AppendLine();
        sb.AppendLine($"#endif  // {guard}");
        File.WriteAllText(Path.Combine(outputDir, headerName), sb.ToString());
    }

    private static void EmitComponentPropertyGetters(StringBuilder sb, ComponentDefModel comp)
    {
        bool any = false;
        for (int i = 0; i < comp.Properties.Count; ++i)
        {
            var prop = comp.Properties[i];
            if (!prop.Scope.IsClientVisible()) continue;
            if (prop.TypeRef is not null) continue;  // containers/struct getter follows
            var cpp = MapDefTypeToCpp(prop.Type);
            if (cpp is null) continue;
            var propName = DefTypeHelper.ToPropertyName(prop.Name);
            bool byRef = cpp == "std::string" || cpp == "std::vector<uint8_t>";
            if (byRef)
            {
                sb.AppendLine($"  const {cpp}& {propName}() const {{");
                sb.AppendLine($"    if (const {cpp}* v = GetScalar<{cpp}>({i})) return *v;");
                sb.AppendLine($"    static const {cpp} kEmpty{{}};");
                sb.AppendLine($"    return kEmpty;");
                sb.AppendLine("  }");
            }
            else
            {
                sb.AppendLine($"  {cpp} {propName}() const {{");
                sb.AppendLine($"    if (const {cpp}* v = GetScalar<{cpp}>({i})) return *v;");
                sb.AppendLine($"    return {cpp}{{}};");
                sb.AppendLine("  }");
            }
            any = true;
        }
        if (any) sb.AppendLine();
    }

    private static void EmitComponentMethodIdxConstants(StringBuilder sb, ComponentDefModel comp)
    {
        bool any = false;
        for (int idx = 0; idx < comp.ClientMethods.Count; ++idx)
        {
            var m = comp.ClientMethods[idx];
            if (!MethodArgsSupported(m)) continue;
            sb.AppendLine($"  static constexpr uint8_t kMethodIdx_{m.Name} = {idx + 1};");
            any = true;
        }
        for (int idx = 0; idx < comp.CellMethods.Count; ++idx)
        {
            var m = comp.CellMethods[idx];
            if (!MethodArgsSupported(m)) continue;
            sb.AppendLine($"  static constexpr uint8_t kMethodIdx_{m.Name} = {idx + 1};");
            any = true;
        }
        for (int idx = 0; idx < comp.BaseMethods.Count; ++idx)
        {
            var m = comp.BaseMethods[idx];
            if (!MethodArgsSupported(m)) continue;
            sb.AppendLine($"  static constexpr uint8_t kMethodIdx_{m.Name} = {idx + 1};");
            any = true;
        }
        if (any) sb.AppendLine();
    }

    private static void EmitComponentUpstreamStubs(StringBuilder sb, ComponentDefModel comp,
                                                    string senderMethod,
                                                    List<MethodDefModel> methods, int direction,
                                                    string @namespace)
    {
        foreach (var m in methods)
        {
            if (!MethodArgsSupported(m)) continue;
            var argList = string.Join(", ",
                m.Args.Select(a => $"{ArgTypeForSignature(a, @namespace)} {a.Name}"));
            sb.AppendLine($"  void {m.Name}({argList}) {{");
            sb.AppendLine("    atlas::ClientEntity* __owner = Owner();");
            sb.AppendLine("    if (__owner == nullptr) return;");
            sb.AppendLine("    atlas::RpcSender* __sender = __owner->Sender();");
            sb.AppendLine("    if (__sender == nullptr) return;");
            sb.AppendLine("    atlas::SpanWriter __writer;");
            foreach (var a in m.Args)
            {
                var ref_ = TypeRefForArg(a);
                var names = new FreshNames();
                EmitWriteValue(sb, "    ", ref_, "__writer", a.Name, @namespace, names);
            }
            // rpc_id = (slot<<24) | (direction<<22) | (entity_type<<8) | method_idx
            sb.AppendLine($"    const uint32_t __rpc_id = (static_cast<uint32_t>(SlotIdx()) << 24)");
            sb.AppendLine($"        | (static_cast<uint32_t>({direction}u) << 22)");
            sb.AppendLine($"        | (static_cast<uint32_t>(__owner->TypeId()) << 8)");
            sb.AppendLine($"        | static_cast<uint32_t>(kMethodIdx_{m.Name});");
            sb.AppendLine($"    __sender->{senderMethod}(__owner->Id(), __rpc_id,");
            sb.AppendLine("        __writer.Bytes().data(), static_cast<int32_t>(__writer.Size()));");
            sb.AppendLine("  }");
            sb.AppendLine();
        }
    }

    private static void EmitComponentDownstreamVirtuals(StringBuilder sb, ComponentDefModel comp,
                                                         string @namespace)
    {
        var supported = comp.ClientMethods.Where(MethodArgsSupported).ToList();
        if (supported.Count == 0) return;
        foreach (var m in supported)
        {
            var argSig = string.Join(", ",
                m.Args.Select(a => $"{ArgTypeForSignature(a, @namespace)} /*{a.Name}*/"));
            sb.AppendLine($"  virtual void {m.Name}({argSig}) {{}}");
        }
        sb.AppendLine();
    }

    private static void EmitComponentDispatchRpc(StringBuilder sb, ComponentDefModel comp,
                                                  string @namespace)
    {
        var supported = comp.ClientMethods.Where(MethodArgsSupported).ToList();
        if (supported.Count == 0) return;
        sb.AppendLine("  bool DispatchRpc(uint32_t __rpc_id, uint64_t /*trace_id*/,");
        sb.AppendLine("                    atlas::SpanReader& __reader) override {");
        sb.AppendLine("    const uint8_t __method = static_cast<uint8_t>(__rpc_id & 0xFFu);");
        sb.AppendLine("    switch (__method) {");
        foreach (var m in supported)
        {
            sb.AppendLine($"      case kMethodIdx_{m.Name}: {{");
            foreach (var a in m.Args)
            {
                var cpp = CppTypeForArg(a, @namespace);
                sb.AppendLine($"        {cpp} {a.Name}{{}};");
                var names = new FreshNames();
                EmitReadValue(sb, "        ", TypeRefForArg(a), "__reader", a.Name, @namespace, names);
            }
            var callArgs = string.Join(", ", m.Args.Select(a => a.Name));
            sb.AppendLine($"        {m.Name}({callArgs});");
            sb.AppendLine("        return true;");
            sb.AppendLine("      }");
        }
        sb.AppendLine("      default: return false;");
        sb.AppendLine("    }");
        sb.AppendLine("  }");
        sb.AppendLine();
    }

    // ── Struct + container property getters ───────────────────────────────

    // Emits one POD C++ struct per `<types><struct>` declaration, with a
    // `FromStructValue` factory that pulls each field out of the variant
    // store. Recursive: struct fields can themselves be containers / nested
    // structs, all handled by EmitExtract.
    // Emits one shared `_Structs.gen.h` covering every linker-collected
    // struct so any entity / component can include it without circular deps.
    // No-op when there are no structs (entity headers still include the
    // empty file via fixed includes; that's a single line guard cost).
    private static void EmitSharedStructs(string outputDir, string @namespace,
                                           IEnumerable<StructDefModel> structs)
    {
        const string headerName = "_Structs.gen.h";
        const string guard = "ATLAS_GEN__STRUCTS_GEN_H_";
        var list = structs.ToList();

        var sb = new StringBuilder();
        sb.AppendLine("// Auto-generated by Atlas.Tools.CppEmitter — do not edit.");
        sb.AppendLine($"#ifndef {guard}");
        sb.AppendLine($"#define {guard}");
        sb.AppendLine();
        sb.AppendLine("#include <cstdint>");
        sb.AppendLine("#include <string>");
        sb.AppendLine("#include <vector>");
        sb.AppendLine();
        sb.AppendLine("#include \"AtlasCore/property_value.h\"");
        sb.AppendLine("#include \"AtlasCore/span_reader.h\"");
        sb.AppendLine("#include \"AtlasCore/span_writer.h\"");
        sb.AppendLine();
        sb.AppendLine($"namespace {@namespace} {{");
        sb.AppendLine();
        if (list.Count > 0)
        {
            foreach (var s in list) sb.AppendLine($"struct {s.Name};");
            sb.AppendLine();
        }
        foreach (var s in list)
        {
            EmitOneStruct(sb, s, @namespace);
        }
        sb.AppendLine($"}}  // namespace {@namespace}");
        sb.AppendLine();
        sb.AppendLine($"#endif  // {guard}");
        File.WriteAllText(Path.Combine(outputDir, headerName), sb.ToString());
    }

    private static void EmitOneStruct(StringBuilder sb, StructDefModel s, string @namespace)
    {
        sb.AppendLine($"struct {s.Name} {{");
        foreach (var f in s.Fields)
        {
            var cpp = CppTypeForRef(f.Type, @namespace);
            sb.AppendLine($"  {cpp} {f.Name}{{}};");
        }
        sb.AppendLine($"  static {s.Name} FromStructValue(const atlas::StructValue& __sv) {{");
        sb.AppendLine($"    {s.Name} __r{{}};");
        sb.AppendLine($"    if (__sv.fields.size() < {s.Fields.Count}) return __r;");
        for (int i = 0; i < s.Fields.Count; ++i)
        {
            var f = s.Fields[i];
            var names = new FreshNames();
            EmitExtract(sb, "    ", f.Type, $"__sv.fields[{i}]", $"__r.{f.Name}", @namespace, names);
        }
        sb.AppendLine($"    return __r;");
        sb.AppendLine("  }");
        sb.AppendLine($"  void Serialize(atlas::SpanWriter& __w) const {{");
        {
            var names = new FreshNames();
            foreach (var f in s.Fields)
                EmitWriteValue(sb, "    ", f.Type, "__w", f.Name, @namespace, names);
        }
        sb.AppendLine("  }");
        sb.AppendLine($"  static bool Deserialize(atlas::SpanReader& __r, {s.Name}& out_) {{");
        {
            var names = new FreshNames();
            foreach (var f in s.Fields)
                EmitReadValue(sb, "    ", f.Type, "__r", $"out_.{f.Name}", @namespace, names);
        }
        sb.AppendLine("    return true;");
        sb.AppendLine("  }");
        sb.AppendLine("};");
        sb.AppendLine();
    }

    // Emits typed getters for client-visible non-scalar properties (struct,
    // list, dict). Each returns by value, materialising a POD/vector tree
    // from the runtime variant store via EmitExtract.
    private static void EmitContainerAndStructGetters(StringBuilder sb, EntityDefModel entity,
                                                      string @namespace)
    {
        var effective = entity.Properties.Where(p => !p.IsReservedPosition).ToList();
        bool any = false;
        for (int i = 0; i < effective.Count; ++i)
        {
            var prop = effective[i];
            if (!prop.Scope.IsClientVisible()) continue;
            if (prop.TypeRef is null) continue;
            var cpp = CppTypeForRef(prop.TypeRef, @namespace);
            var propName = DefTypeHelper.ToPropertyName(prop.Name);
            sb.AppendLine($"  {cpp} {propName}() const {{");
            sb.AppendLine($"    {cpp} __r{{}};");
            var names = new FreshNames();
            // Top-level container/struct sits in properties_[i] directly.
            EmitExtract(sb, "    ", prop.TypeRef, $"Properties()[{i}]", "__r", @namespace, names);
            sb.AppendLine($"    return __r;");
            sb.AppendLine("  }");
            any = true;
        }
        if (any) sb.AppendLine();
    }

    // Mints unique local names within one extract subtree so nested
    // recursions don't shadow each other.
    private sealed class FreshNames
    {
        private int _n;
        public string Next(string prefix) => $"{prefix}{_n++}";
    }

    // Recursive: emit code that, given a PropertyValue expression `pvExpr`,
    // unpacks its content into typed local/field `destExpr`. Scalars use
    // std::get_if; struct delegates to the codegen-emitted FromStructValue;
    // list/dict iterate the runtime container and recurse element-by-element.
    private static void EmitExtract(StringBuilder sb, string indent, DataTypeRefModel typeRef,
                                     string pvExpr, string destExpr, string @namespace,
                                     FreshNames names)
    {
        switch (typeRef.Kind)
        {
            case PropertyDataKind.Struct:
            {
                var sv = names.Next("__sv");
                sb.AppendLine($"{indent}if (auto* {sv} = std::get_if<std::unique_ptr<atlas::StructValue>>(&{pvExpr}))");
                sb.AppendLine($"{indent}  {destExpr} = {@namespace}::{typeRef.StructName}::FromStructValue(**{sv});");
                break;
            }
            case PropertyDataKind.List:
            {
                var lv = names.Next("__lv");
                var it = names.Next("__it");
                var elem = names.Next("__e");
                var elemCpp = CppTypeForRef(typeRef.Elem!, @namespace);
                sb.AppendLine($"{indent}if (auto* {lv} = std::get_if<std::unique_ptr<atlas::ListValue>>(&{pvExpr})) {{");
                sb.AppendLine($"{indent}  {destExpr}.reserve((**{lv}).items.size());");
                sb.AppendLine($"{indent}  for (const auto& {it} : (**{lv}).items) {{");
                sb.AppendLine($"{indent}    {elemCpp} {elem}{{}};");
                EmitExtract(sb, indent + "    ", typeRef.Elem!, it, elem, @namespace, names);
                sb.AppendLine($"{indent}    {destExpr}.push_back(std::move({elem}));");
                sb.AppendLine($"{indent}  }}");
                sb.AppendLine($"{indent}}}");
                break;
            }
            case PropertyDataKind.Dict:
            {
                var dv = names.Next("__dv");
                var ent = names.Next("__ent");
                var k = names.Next("__k");
                var v = names.Next("__v");
                var keyCpp = CppTypeForRef(typeRef.Key!, @namespace);
                var valCpp = CppTypeForRef(typeRef.Elem!, @namespace);
                sb.AppendLine($"{indent}if (auto* {dv} = std::get_if<std::unique_ptr<atlas::DictValue>>(&{pvExpr})) {{");
                sb.AppendLine($"{indent}  {destExpr}.reserve((**{dv}).entries.size());");
                sb.AppendLine($"{indent}  for (const auto& {ent} : (**{dv}).entries) {{");
                sb.AppendLine($"{indent}    {keyCpp} {k}{{}};");
                sb.AppendLine($"{indent}    {valCpp} {v}{{}};");
                EmitExtract(sb, indent + "    ", typeRef.Key!, $"{ent}.first", k, @namespace, names);
                EmitExtract(sb, indent + "    ", typeRef.Elem!, $"{ent}.second", v, @namespace, names);
                sb.AppendLine($"{indent}    {destExpr}.emplace_back(std::move({k}), std::move({v}));");
                sb.AppendLine($"{indent}  }}");
                sb.AppendLine($"{indent}}}");
                break;
            }
            default:
            {
                var cpp = MapScalarKindToCpp(typeRef.Kind);
                if (cpp is null) return;  // unsupported scalar, skip extract
                var v = names.Next("__v");
                sb.AppendLine($"{indent}if (const auto* {v} = std::get_if<{cpp}>(&{pvExpr})) {destExpr} = *{v};");
                break;
            }
        }
    }

    private static string CppTypeForRef(DataTypeRefModel r, string @namespace)
    {
        return r.Kind switch
        {
            PropertyDataKind.Struct => $"{@namespace}::{r.StructName}",
            PropertyDataKind.List => $"std::vector<{CppTypeForRef(r.Elem!, @namespace)}>",
            PropertyDataKind.Dict =>
                $"std::vector<std::pair<{CppTypeForRef(r.Key!, @namespace)}, {CppTypeForRef(r.Elem!, @namespace)}>>",
            _ => MapScalarKindToCpp(r.Kind) ?? "void",
        };
    }

    private static string? MapScalarKindToCpp(PropertyDataKind kind) => kind switch
    {
        PropertyDataKind.Bool => "bool",
        PropertyDataKind.Int8 => "int8_t",
        PropertyDataKind.UInt8 => "uint8_t",
        PropertyDataKind.Int16 => "int16_t",
        PropertyDataKind.UInt16 => "uint16_t",
        PropertyDataKind.Int32 => "int32_t",
        PropertyDataKind.UInt32 => "uint32_t",
        PropertyDataKind.Int64 => "int64_t",
        PropertyDataKind.UInt64 => "uint64_t",
        PropertyDataKind.Float => "float",
        PropertyDataKind.Double => "double",
        PropertyDataKind.String => "std::string",
        PropertyDataKind.Bytes => "std::vector<uint8_t>",
        PropertyDataKind.Vector3 => "atlas::Vec3",
        PropertyDataKind.Quaternion => "atlas::Quat",
        _ => null,
    };

    private static string? MapDefTypeToCpp(string defType)
    {
        return defType.ToLowerInvariant() switch
        {
            "bool" => "bool",
            "int8" => "int8_t",
            "uint8" => "uint8_t",
            "int16" => "int16_t",
            "uint16" => "uint16_t",
            "int32" => "int32_t",
            "uint32" => "uint32_t",
            "int64" => "int64_t",
            "uint64" => "uint64_t",
            "float" => "float",
            "double" => "double",
            "string" => "std::string",
            "bytes" => "std::vector<uint8_t>",
            "vector3" => "atlas::Vec3",
            "quaternion" => "atlas::Quat",
            _ => null,
        };
    }

    // Recursive integral write: scalar / struct.Serialize / list+dict count+loop.
    private static void EmitWriteValue(StringBuilder sb, string indent, DataTypeRefModel typeRef,
                                        string writerVar, string valueExpr,
                                        string @namespace, FreshNames names)
    {
        switch (typeRef.Kind)
        {
            case PropertyDataKind.Struct:
                sb.AppendLine($"{indent}{valueExpr}.Serialize({writerVar});");
                break;
            case PropertyDataKind.List:
            {
                var iter = names.Next("__i");
                sb.AppendLine($"{indent}{writerVar}.Write(static_cast<uint16_t>({valueExpr}.size()));");
                sb.AppendLine($"{indent}for (const auto& {iter} : {valueExpr}) {{");
                EmitWriteValue(sb, indent + "  ", typeRef.Elem!, writerVar, iter, @namespace, names);
                sb.AppendLine($"{indent}}}");
                break;
            }
            case PropertyDataKind.Dict:
            {
                var ent = names.Next("__e");
                sb.AppendLine($"{indent}{writerVar}.Write(static_cast<uint16_t>({valueExpr}.size()));");
                sb.AppendLine($"{indent}for (const auto& {ent} : {valueExpr}) {{");
                EmitWriteValue(sb, indent + "  ", typeRef.Key!, writerVar, $"{ent}.first", @namespace, names);
                EmitWriteValue(sb, indent + "  ", typeRef.Elem!, writerVar, $"{ent}.second", @namespace, names);
                sb.AppendLine($"{indent}}}");
                break;
            }
            case PropertyDataKind.Bool:
                sb.AppendLine($"{indent}{writerVar}.Write(static_cast<uint8_t>({valueExpr} ? 1 : 0));");
                break;
            case PropertyDataKind.String:
                sb.AppendLine($"{indent}{writerVar}.WriteString({valueExpr});");
                break;
            case PropertyDataKind.Bytes:
                sb.AppendLine($"{indent}{writerVar}.WriteBytes({valueExpr});");
                break;
            case PropertyDataKind.Vector3:
                sb.AppendLine($"{indent}{writerVar}.WriteVec3({valueExpr});");
                break;
            case PropertyDataKind.Quaternion:
                sb.AppendLine($"{indent}{writerVar}.WriteQuat({valueExpr});");
                break;
            default:
                sb.AppendLine($"{indent}{writerVar}.Write({valueExpr});");
                break;
        }
    }

    // Recursive integral read. All failure paths emit `return false;` and
    // rely on the enclosing function (DispatchRpc, Deserialize) returning bool.
    private static void EmitReadValue(StringBuilder sb, string indent, DataTypeRefModel typeRef,
                                       string readerVar, string destExpr,
                                       string @namespace, FreshNames names)
    {
        switch (typeRef.Kind)
        {
            case PropertyDataKind.Struct:
                sb.AppendLine($"{indent}if (!{@namespace}::{typeRef.StructName}::Deserialize({readerVar}, {destExpr})) return false;");
                break;
            case PropertyDataKind.List:
            {
                var count = names.Next("__c");
                var i = names.Next("__i");
                var elem = names.Next("__e");
                var elemCpp = CppTypeForRef(typeRef.Elem!, @namespace);
                sb.AppendLine($"{indent}{{");
                sb.AppendLine($"{indent}  uint16_t {count} = 0;");
                sb.AppendLine($"{indent}  if (!{readerVar}.Read({count})) return false;");
                sb.AppendLine($"{indent}  {destExpr}.reserve({count});");
                sb.AppendLine($"{indent}  for (uint16_t {i} = 0; {i} < {count}; ++{i}) {{");
                sb.AppendLine($"{indent}    {elemCpp} {elem}{{}};");
                EmitReadValue(sb, indent + "    ", typeRef.Elem!, readerVar, elem, @namespace, names);
                sb.AppendLine($"{indent}    {destExpr}.push_back(std::move({elem}));");
                sb.AppendLine($"{indent}  }}");
                sb.AppendLine($"{indent}}}");
                break;
            }
            case PropertyDataKind.Dict:
            {
                var count = names.Next("__c");
                var i = names.Next("__i");
                var k = names.Next("__k");
                var v = names.Next("__v");
                var keyCpp = CppTypeForRef(typeRef.Key!, @namespace);
                var valCpp = CppTypeForRef(typeRef.Elem!, @namespace);
                sb.AppendLine($"{indent}{{");
                sb.AppendLine($"{indent}  uint16_t {count} = 0;");
                sb.AppendLine($"{indent}  if (!{readerVar}.Read({count})) return false;");
                sb.AppendLine($"{indent}  {destExpr}.reserve({count});");
                sb.AppendLine($"{indent}  for (uint16_t {i} = 0; {i} < {count}; ++{i}) {{");
                sb.AppendLine($"{indent}    {keyCpp} {k}{{}};");
                sb.AppendLine($"{indent}    {valCpp} {v}{{}};");
                EmitReadValue(sb, indent + "    ", typeRef.Key!, readerVar, k, @namespace, names);
                EmitReadValue(sb, indent + "    ", typeRef.Elem!, readerVar, v, @namespace, names);
                sb.AppendLine($"{indent}    {destExpr}.emplace_back(std::move({k}), std::move({v}));");
                sb.AppendLine($"{indent}  }}");
                sb.AppendLine($"{indent}}}");
                break;
            }
            case PropertyDataKind.Bool:
            {
                var tmp = names.Next("__b");
                sb.AppendLine($"{indent}{{ uint8_t {tmp} = 0; if (!{readerVar}.Read({tmp})) return false; {destExpr} = ({tmp} != 0); }}");
                break;
            }
            case PropertyDataKind.String:
                sb.AppendLine($"{indent}if (!{readerVar}.ReadString({destExpr})) return false;");
                break;
            case PropertyDataKind.Bytes:
                sb.AppendLine($"{indent}if (!{readerVar}.ReadBytes({destExpr})) return false;");
                break;
            case PropertyDataKind.Vector3:
                sb.AppendLine($"{indent}if (!{readerVar}.ReadVec3({destExpr})) return false;");
                break;
            case PropertyDataKind.Quaternion:
                sb.AppendLine($"{indent}if (!{readerVar}.ReadQuat({destExpr})) return false;");
                break;
            default:
                sb.AppendLine($"{indent}if (!{readerVar}.Read({destExpr})) return false;");
                break;
        }
    }

    private static void FlushDiagnostics(List<Diagnostic> ds)
    {
        foreach (var d in ds)
        {
            Console.Error.WriteLine(d.ToString());
        }
        ds.Clear();
    }

    private static bool TryReadValue(string[] args, ref int i, string flag, out string value)
    {
        if (i + 1 >= args.Length)
        {
            Console.Error.WriteLine($"CppEmitter: {flag} requires a value");
            value = string.Empty;
            return false;
        }
        value = args[++i];
        return true;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("Usage: Atlas.Tools.CppEmitter --entity-defs <dir> --output <dir> [--namespace <ns>]");
        Console.WriteLine("  --entity-defs DIR    Directory containing *.def + entity_ids.xml");
        Console.WriteLine("  --output DIR         Output directory for .gen.h files");
        Console.WriteLine("  --namespace NS       C++ namespace for emitted classes (default: atlas::mvp)");
        Console.WriteLine("  --help, -h           Show this message");
    }
}
