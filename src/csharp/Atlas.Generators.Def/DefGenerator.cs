using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Threading;
using Atlas.Generators.Def.Emitters;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Atlas.Generators.Def;

[Generator(LanguageNames.CSharp)]
public sealed class DefGenerator : IIncrementalGenerator
{
    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        // 1. Read .def files. Each one is either an entity or a
        // standalone component, dispatched by root element.
        var defs = context.AdditionalTextsProvider
            .Where(static f => f.Path.EndsWith(".def", StringComparison.OrdinalIgnoreCase))
            .Select(static (f, ct) =>
            {
                var text = f.GetText(ct);
                if (text == null) return null;
                return DefParser.ParseAny(text, f.Path, null);
            })
            .Where(static m => m is not null)
            .Select(static (m, _) => m!);

        // 1b. Read entity_ids.xml manifest(s). At most one is expected per
        // compilation; the manifest is the source of truth for type_id when
        // it is provided. .def files in the same compilation must drop their
        // inline `id` attribute.
        var manifestSources = context.AdditionalTextsProvider
            .Where(static f => string.Equals(
                System.IO.Path.GetFileName(f.Path),
                EntityIdManifestParser.FileName,
                StringComparison.OrdinalIgnoreCase))
            .Select(static (f, ct) =>
            {
                var text = f.GetText(ct);
                return text == null ? null : new ManifestSource(f.Path, text.ToString());
            })
            .Where(static m => m is not null)
            .Select(static (m, _) => m!);

        // 2. Read process context from preprocessor symbols
        var processCtx = context.CompilationProvider
            .Select(static (c, _) =>
            {
                if (c.SyntaxTrees.FirstOrDefault() is { } tree)
                {
                    var parseOpts = tree.Options as Microsoft.CodeAnalysis.CSharp.CSharpParseOptions;
                    if (parseOpts != null)
                    {
                        var syms = parseOpts.PreprocessorSymbolNames;
                        if (syms.Contains("ATLAS_BASE"))   return ProcessContext.Base;
                        if (syms.Contains("ATLAS_CELL"))   return ProcessContext.Cell;
                        if (syms.Contains("ATLAS_CLIENT")) return ProcessContext.Client;
                    }
                }
                return ProcessContext.Server;
            });

        // 3. Find user [Entity("Name")] classes
        var userEntities = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                "Atlas.Entity.EntityAttribute",
                static (node, _) => node is ClassDeclarationSyntax,
                static (ctx, _) =>
                {
                    var classSymbol = (INamedTypeSymbol)ctx.TargetSymbol;
                    var attr = classSymbol.GetAttributes().FirstOrDefault(a =>
                        a.AttributeClass?.Name == "EntityAttribute");
                    var typeName = attr?.ConstructorArguments.FirstOrDefault().Value?.ToString() ?? "";
                    return new UserEntityInfo(
                        classSymbol.Name,
                        classSymbol.ContainingNamespace?.ToDisplayString() ?? "",
                        typeName,
                        classSymbol.BaseType?.Name ?? "");
                });

        // 4. Combine and generate
        var combined = defs.Collect()
            .Combine(userEntities.Collect())
            .Combine(processCtx)
            .Combine(manifestSources.Collect());

        context.RegisterSourceOutput(combined, Execute);
    }

    private static void Execute(SourceProductionContext spc,
        (((ImmutableArray<ParsedDef> Defs, ImmutableArray<UserEntityInfo> Users) DefsAndUsers,
          ProcessContext Ctx),
         ImmutableArray<ManifestSource> Manifests) input)
    {
        var ((defsAndUsers, ctx), manifestSources) = input;
        var (parsed, users) = defsAndUsers;

        if (parsed.IsDefaultOrEmpty || users.IsDefaultOrEmpty)
            return;

        // Split entities and standalone components — both come down the
        // same .def pipeline; the parser flagged which is which by root.
        var defs = parsed.Where(p => p.Entity != null).Select(p => p.Entity!).ToImmutableArray();
        var standaloneComponents = parsed.Where(p => p.StandaloneComponent != null)
                                          .Select(p => p.StandaloneComponent!).ToList();

        // Build def lookup.
        //
        // Surface parser-stage diagnostics flagged on the model. DefParser
        // currently detects ATLAS_DEF008 (reserved `position`) during parse
        // and records the decision on the PropertyDefModel; we report it
        // here because the parser runs inside the incremental pipeline's
        // Select transform where no SourceProductionContext is available.
        var defMap = new Dictionary<string, EntityDefModel>();
        foreach (var def in defs)
        {
            if (!string.IsNullOrEmpty(def.Name))
                defMap[def.Name] = def;

            foreach (var prop in def.Properties)
            {
                if (prop.IsReservedPosition)
                {
                    spc.ReportDiagnostic(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF008, Location.None, def.Name));
                }
            }
        }

        // Resolve type_id from entity_ids.xml manifest when present.
        // Inline `id` on .def is reserved as a fallback for unit-test
        // pipelines that feed XML through DefParser without an AdditionalFile
        // for the manifest.
        var manifest = DefGeneratorHelpers.ResolveManifest(manifestSources, spc);
        var typeIndexMap = DefGeneratorHelpers.BuildTypeIndexMap(defs, manifest, spc);

        // Determine base class name based on context
        var baseClass = ctx switch
        {
            ProcessContext.Client => "Atlas.Client.ClientEntity",
            _ => "Atlas.Entity.ServerEntity",
        };
        var baseClassShort = ctx switch
        {
            ProcessContext.Client => "ClientEntity",
            _ => "ServerEntity",
        };

        var entityList = new List<(EntityDefModel Def, string ClassName, string Namespace)>();

        // Link pass: dedup structs, assign stable ids, resolve aliases,
        // resolve component inheritance + cross-entity references. The
        // emitters below consume linked.StructsByName instead of
        // reassembling the map from defs[] — keeps DefLinker the
        // single source of truth for which struct / component names
        // are addressable.
        var linked = DefLinker.Link(defs.ToList(), standaloneComponents, spc.ReportDiagnostic);
        if (linked is null) return;
        var structsByName = linked.StructsByName;

        // Match user entities to defs and generate per-entity code
        foreach (var user in users)
        {
            if (!defMap.TryGetValue(user.TypeName, out var def))
            {
                spc.ReportDiagnostic(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF001, Location.None, user.TypeName));
                continue;
            }

            entityList.Add((def, user.ClassName, user.Namespace));

            // Properties (fields, dirty tracking, change callbacks)
            var properties = PropertiesEmitter.Emit(def, user.ClassName, user.Namespace, ctx, structsByName);
            if (properties != null)
                spc.AddSource($"{user.ClassName}.Properties.g.cs", SourceText.From(properties, System.Text.Encoding.UTF8));

            // Serialization (Serialize/Deserialize + TypeName + TypeId)
            var serialization = SerializationEmitter.Emit(def, user.ClassName, user.Namespace, ctx, typeIndexMap);
            spc.AddSource($"{user.ClassName}.Serialization.g.cs", SourceText.From(serialization, System.Text.Encoding.UTF8));

            // DeltaSync (delta/owner/other sync)
            var deltaSync = DeltaSyncEmitter.Emit(def, user.ClassName, user.Namespace, ctx);
            if (deltaSync != null)
                spc.AddSource($"{user.ClassName}.DeltaSync.g.cs", SourceText.From(deltaSync, System.Text.Encoding.UTF8));

            // RPC stubs
            var stubs = RpcStubEmitter.Emit(def, user.ClassName, user.Namespace, baseClassShort, ctx, typeIndexMap);
            if (!string.IsNullOrEmpty(stubs))
                spc.AddSource($"{user.ClassName}.RpcStubs.g.cs", SourceText.From(stubs, System.Text.Encoding.UTF8));

            // Mailboxes
            var mailboxes = MailboxEmitter.Emit(def, user.ClassName, user.Namespace, baseClassShort, ctx, typeIndexMap);
            if (!string.IsNullOrEmpty(mailboxes))
                spc.AddSource($"{user.ClassName}.Mailboxes.g.cs", SourceText.From(mailboxes, System.Text.Encoding.UTF8));

            // Component slot accessors. Server emits write-side helpers
            // (HasOwnerDirtyComponent / WriteOwnerComponentSection /
            // ClearDirtyComponents); client emits the read-side
            // ApplyComponentSection. Both sides emit ResolveSyncedSlot
            // and the typed accessor properties.
            var accessors = EntityComponentAccessorEmitter.Emit(def, user.ClassName, user.Namespace, ctx);
            if (accessors != null)
                spc.AddSource($"{user.ClassName}.Components.g.cs",
                              SourceText.From(accessors, System.Text.Encoding.UTF8));
        }

        // Component class emission. Each distinct TypeName is emitted
        // exactly once — standalone defs win over inline (since they're
        // the canonical type) and inline-only types still emit as one-
        // off classes. A type is "leaf" (sealed) iff no other component
        // extends it.
        var emittedComponentTypes = new HashSet<string>(StringComparer.Ordinal);
        var allComponents = new List<ComponentDefModel>();
        // Standalone first — canonical type definitions.
        allComponents.AddRange(standaloneComponents);
        // Then inline-from-entities, but only those NOT shadowed by a
        // standalone of the same name.
        foreach (var def in defs)
        {
            foreach (var c in def.Components)
            {
                if (c.IsStandalone) continue;
                if (allComponents.Exists(s => s.TypeName == c.TypeName)) continue;
                allComponents.Add(c);
            }
        }

        var nonLeafTypes = new HashSet<string>(StringComparer.Ordinal);
        foreach (var c in allComponents)
        {
            if (!string.IsNullOrEmpty(c.BaseTypeName)) nonLeafTypes.Add(c.BaseTypeName!);
        }

        // Server inherits Atlas.Entity.Components.ReplicatedComponent;
        // Client inherits Atlas.Components.ClientReplicatedComponent.
        // ComponentEmitter switches on ctx so each side emits the right
        // base reference + only the methods it needs (server: WriteOwnerDelta;
        // client: ApplyDelta).
        foreach (var c in allComponents)
        {
            if (c.Locality != ComponentLocality.Synced) continue;
            if (!emittedComponentTypes.Add(c.TypeName)) continue;
            bool isLeaf = !nonLeafTypes.Contains(c.TypeName);
            var src = ComponentEmitter.Emit(c, isLeaf, ctx);
            spc.AddSource($"{c.TypeName}.Component.g.cs",
                          SourceText.From(src, System.Text.Encoding.UTF8));
        }

        // Global: EntityFactory
        if (entityList.Count > 0)
        {
            var factory = Emitters.FactoryEmitter.Emit(entityList, typeIndexMap, ctx);
            spc.AddSource("EntityFactory.g.cs", SourceText.From(factory, System.Text.Encoding.UTF8));
        }

        // Global: RPC IDs
        var rpcIds = RpcIdEmitter.Emit(defs.ToList(), typeIndexMap);
        spc.AddSource("RpcIds.g.cs", SourceText.From(rpcIds, System.Text.Encoding.UTF8));

        // Global: Dispatcher
        if (entityList.Count > 0)
        {
            var dispatchBase = ctx == ProcessContext.Client ? "Atlas.Client.ClientEntity" : "Atlas.Entity.ServerEntity";
            var dispatcher = DispatcherEmitter.Emit(entityList, ctx, typeIndexMap, dispatchBase, allComponents);
            spc.AddSource("DefRpcDispatcher.g.cs", SourceText.From(dispatcher, System.Text.Encoding.UTF8));
        }

        // Per-struct code: partial struct body with field members and the
        // whole-struct Serialize/Deserialize pair. Emitted into the
        // Atlas.Def namespace so different entity defs can share a struct
        // identity without namespace shopping.
        foreach (var s in linked.Structs)
        {
            var structCode = Emitters.StructEmitter.Emit(s, spc.ReportDiagnostic);
            spc.AddSource($"{s.Name}.Struct.g.cs",
                          SourceText.From(structCode, System.Text.Encoding.UTF8));
        }

        // Global: StructRegistry — registers every <struct> before any
        // entity type, so RegisterType's type_ref resolver sees a populated
        // struct table.
        if (linked.Structs.Count > 0)
        {
            var structRegistry = Emitters.StructRegistryEmitter.Emit(linked.Structs, ctx);
            spc.AddSource("DefStructRegistry.g.cs",
                          SourceText.From(structRegistry, System.Text.Encoding.UTF8));
        }

        // Global: TypeRegistry — serializes entity descriptors (with RPC + ExposedScope) to C++
        if (entityList.Count > 0)
        {
            var typeRegistry = Emitters.TypeRegistryEmitter.Emit(entityList, typeIndexMap, ctx);
            spc.AddSource("DefEntityTypeRegistry.g.cs", SourceText.From(typeRegistry, System.Text.Encoding.UTF8));
        }

        // EntityDef digest: SHA-256 of normalized entity/struct/component
        // surface so client and server reject mismatched .def builds at the
        // login handshake.
        if (entityList.Count > 0 || linked.Structs.Count > 0 || standaloneComponents.Count > 0)
        {
            var digest = Emitters.DigestEmitter.Emit(defs, linked.Structs, standaloneComponents,
                                                    typeIndexMap);
            spc.AddSource("EntityDefDigest.g.cs",
                          SourceText.From(digest, System.Text.Encoding.UTF8));
        }

        // Single ModuleInitializer entry: replaces per-emitter ones so the
        // four registration steps run in a fixed order within the assembly.
        bool hasStructs = linked.Structs.Count > 0;
        bool hasEntities = entityList.Count > 0;
        bool hasDispatcher = entityList.Count > 0;
        if (hasStructs || hasEntities || hasDispatcher)
        {
            var bootstrap = Emitters.BootstrapEmitter.Emit(hasStructs, hasEntities, hasDispatcher, ctx);
            spc.AddSource("DefBootstrap.g.cs",
                          SourceText.From(bootstrap, System.Text.Encoding.UTF8));
        }
    }
}

internal static class DefGeneratorHelpers
{
    public static EntityIdManifest? ResolveManifest(ImmutableArray<ManifestSource> sources,
                                                    SourceProductionContext spc)
    {
        if (sources.IsDefaultOrEmpty) return null;
        if (sources.Length > 1)
        {
            var paths = string.Join(", ", sources.Select(s => s.Path));
            spc.ReportDiagnostic(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF025, Location.None, paths,
                "multiple entity_ids.xml manifests in compilation; expected exactly one"));
        }
        var primary = sources[0];
        return EntityIdManifestParser.Parse(primary.Xml, primary.Path, spc.ReportDiagnostic);
    }

    public static Dictionary<string, ushort> BuildTypeIndexMap(IEnumerable<EntityDefModel> defs,
                                                               EntityIdManifest? manifest,
                                                               SourceProductionContext spc)
    {
        var map = new Dictionary<string, ushort>(StringComparer.Ordinal);
        var owners = new Dictionary<ushort, string>();
        foreach (var def in defs)
        {
            if (string.IsNullOrEmpty(def.Name)) continue;
            if (!TryResolveId(def, manifest, spc, out var id)) continue;
            if (owners.TryGetValue(id, out var prev) && prev != def.Name)
            {
                spc.ReportDiagnostic(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF021, Location.None, def.Name, id, prev));
                continue;
            }
            owners[id] = def.Name;
            map[def.Name] = id;
        }
        return map;
    }

    private static bool TryResolveId(EntityDefModel def, EntityIdManifest? manifest,
                                     SourceProductionContext spc, out ushort id)
    {
        id = 0;
        if (manifest == null)
        {
            spc.ReportDiagnostic(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF019, Location.None, def.Name));
            return false;
        }
        if (manifest.DeprecatedNames.Contains(def.Name))
        {
            spc.ReportDiagnostic(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF023, Location.None, def.Name, manifest.SourcePath));
            return false;
        }
        if (!manifest.ActiveByName.TryGetValue(def.Name, out id))
        {
            spc.ReportDiagnostic(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF024, Location.None, def.Name, manifest.SourcePath));
            return false;
        }
        return true;
    }
}

internal sealed class UserEntityInfo
{
    public string ClassName { get; }
    public string Namespace { get; }
    public string TypeName { get; }
    public string BaseClassName { get; }

    public UserEntityInfo(string className, string ns, string typeName, string baseClassName)
    {
        ClassName = className;
        Namespace = ns;
        TypeName = typeName;
        BaseClassName = baseClassName;
    }
}

internal sealed class ManifestSource
{
    public string Path { get; }
    public string Xml { get; }

    public ManifestSource(string path, string xml)
    {
        Path = path;
        Xml = xml;
    }
}
