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

        // Manifest owns type_id when present; .def must drop inline `id`.
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

        // Same shape as entity_ids.xml; assigns wire-stable component_type_id.
        var componentManifestSources = context.AdditionalTextsProvider
            .Where(static f => string.Equals(
                System.IO.Path.GetFileName(f.Path),
                ComponentIdManifestParser.FileName,
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
            .Combine(manifestSources.Collect())
            .Combine(componentManifestSources.Collect());

        context.RegisterSourceOutput(combined, Execute);
    }

    private static void Execute(SourceProductionContext spc,
        ((((ImmutableArray<ParsedDef> Defs, ImmutableArray<UserEntityInfo> Users) DefsAndUsers,
           ProcessContext Ctx),
          ImmutableArray<ManifestSource> Manifests),
         ImmutableArray<ManifestSource> ComponentManifests) input)
    {
        var (((defsAndUsers, ctx), manifestSources), componentManifestSources) = input;
        var (parsed, users) = defsAndUsers;

        if (parsed.IsDefaultOrEmpty)
            return;

        // Split entities, standalone components, and space-data manifests
        // — parser flagged each by root element.
        var defs = parsed.Where(p => p.Entity != null).Select(p => p.Entity!).ToImmutableArray();
        var standaloneComponents = parsed.Where(p => p.StandaloneComponent != null)
                                          .Select(p => p.StandaloneComponent!).ToList();
        var spaceDataDefs = parsed.Where(p => p.SpaceData != null)
                                   .Select(p => p.SpaceData!).ToList();

        // SpaceData keys: merge across all .def files, dedup by id+name,
        // emit a single global registry independent of user [Entity] classes.
        if (spaceDataDefs.Count > 0)
        {
            var seenIds = new HashSet<ushort>();
            var seenNames = new HashSet<string>(StringComparer.Ordinal);
            var merged = new List<SpaceDataKeyDefModel>();
            foreach (var def in spaceDataDefs)
            {
                foreach (var key in def.Keys)
                {
                    if (!seenIds.Add(key.KeyId) || !seenNames.Add(key.Name))
                    {
                        spc.ReportDiagnostic(Diagnostic.Create(
                            DefDiagnosticDescriptors.DEF006, Location.None,
                            def.SourcePath,
                            $"duplicate SpaceData key '{key.Name}' / id={key.KeyId}"));
                        continue;
                    }
                    merged.Add(key);
                }
            }
            var spaceDataSrc = Emitters.SpaceDataEmitter.Emit(merged);
            spc.AddSource("SpaceDataKeys.g.cs",
                          SourceText.From(spaceDataSrc, System.Text.Encoding.UTF8));
        }

        if (users.IsDefaultOrEmpty)
            return;

        // Parser-stage diagnostics surface here because DefParser ran in a
        // Select transform without SourceProductionContext access.
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

        // Inline `id` on .def is a unit-test fallback when no manifest is wired.
        var manifest = DefGeneratorHelpers.ResolveManifest(manifestSources, spc);
        var typeIndexMap = DefGeneratorHelpers.BuildTypeIndexMap(defs, manifest, spc);

        // Per-process base class — cell vs base see disjoint API surfaces.
        var baseClass = ctx switch
        {
            ProcessContext.Client => "Atlas.Client.ClientEntity",
            ProcessContext.Base => "Atlas.Entity.BaseServerEntity",
            ProcessContext.Cell => "Atlas.Entity.CellServerEntity",
            _ => "Atlas.Entity.ServerEntity",
        };
        var baseClassShort = ctx switch
        {
            ProcessContext.Client => "ClientEntity",
            ProcessContext.Base => "BaseServerEntity",
            ProcessContext.Cell => "CellServerEntity",
            _ => "ServerEntity",
        };

        var entityList = new List<(EntityDefModel Def, string ClassName, string Namespace)>();

        // DefLinker is the single source of truth for addressable struct /
        // component names; emitters consume linked.StructsByName below.
        var componentManifest =
            DefGeneratorHelpers.ResolveComponentManifest(componentManifestSources, spc);
        var linked = DefLinker.Link(defs.ToList(), standaloneComponents, componentManifest,
                                     spc.ReportDiagnostic);
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

            // Server: dirty-tracking + WriteOwnerComponentSection; client:
            // ApplyComponentSection. Both sides get typed slot accessors.
            var accessors = EntityComponentAccessorEmitter.Emit(def, user.ClassName, user.Namespace, ctx);
            if (accessors != null)
                spc.AddSource($"{user.ClassName}.Components.g.cs",
                              SourceText.From(accessors, System.Text.Encoding.UTF8));
        }

        // One class per distinct TypeName; standalone wins over inline.
        // A type is "leaf" (sealed) iff no other component extends it.
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

        // Per-ctx base class + method set (server: WriteOwnerDelta; client: ApplyDelta).
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

        // Emitted under Atlas.Def so different entity defs share struct identity.
        foreach (var s in linked.Structs)
        {
            var structCode = Emitters.StructEmitter.Emit(s, spc.ReportDiagnostic);
            spc.AddSource($"{s.Name}.Struct.g.cs",
                          SourceText.From(structCode, System.Text.Encoding.UTF8));
        }

        // Must run before entity types — RegisterType resolves type_refs eagerly.
        if (linked.Structs.Count > 0)
        {
            var structRegistry = Emitters.StructRegistryEmitter.Emit(linked.Structs, ctx);
            spc.AddSource("DefStructRegistry.g.cs",
                          SourceText.From(structRegistry, System.Text.Encoding.UTF8));
        }

        // Order: StructRegistry → ComponentRegistry → TypeRegistry
        // (slot tables reference component_type_id; component props reference struct ids).
        var hasSyncedStandaloneComponents = standaloneComponents.Any(
            c => c.Locality == ComponentLocality.Synced && c.ComponentTypeId > 0);
        if (hasSyncedStandaloneComponents)
        {
            var componentRegistry = Emitters.ComponentRegistryEmitter.Emit(standaloneComponents, ctx);
            spc.AddSource("DefComponentRegistry.g.cs",
                          SourceText.From(componentRegistry, System.Text.Encoding.UTF8));
        }

        // Global: TypeRegistry — serializes entity descriptors (with RPC + ExposedScope) to C++
        if (entityList.Count > 0)
        {
            var typeRegistry = Emitters.TypeRegistryEmitter.Emit(entityList, typeIndexMap, ctx);
            spc.AddSource("DefEntityTypeRegistry.g.cs", SourceText.From(typeRegistry, System.Text.Encoding.UTF8));
        }

        // SHA-256 of the surface; BaseApp bounces mismatched builds at login.
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
        bool hasComponents = hasSyncedStandaloneComponents;
        bool hasEntities = entityList.Count > 0;
        bool hasDispatcher = entityList.Count > 0;
        if (hasStructs || hasComponents || hasEntities || hasDispatcher)
        {
            var bootstrap = Emitters.BootstrapEmitter.Emit(hasStructs, hasComponents, hasEntities,
                                                          hasDispatcher, ctx);
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

    public static ComponentIdManifest? ResolveComponentManifest(
        ImmutableArray<ManifestSource> sources, SourceProductionContext spc)
    {
        if (sources.IsDefaultOrEmpty) return null;
        if (sources.Length > 1)
        {
            var paths = string.Join(", ", sources.Select(s => s.Path));
            spc.ReportDiagnostic(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF025, Location.None, paths,
                "multiple component_ids.xml manifests in compilation; expected exactly one"));
        }
        var primary = sources[0];
        return ComponentIdManifestParser.Parse(primary.Xml, primary.Path, spc.ReportDiagnostic);
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
