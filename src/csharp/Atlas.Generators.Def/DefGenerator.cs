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
        // 1. Read .def files
        var defs = context.AdditionalTextsProvider
            .Where(static f => f.Path.EndsWith(".def", StringComparison.OrdinalIgnoreCase))
            .Select(static (f, ct) =>
            {
                var text = f.GetText(ct);
                if (text == null) return null;
                return DefParser.Parse(text, f.Path, null);
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
            .Combine(processCtx);

        context.RegisterSourceOutput(combined, Execute);
    }

    private static void Execute(SourceProductionContext spc,
        ((ImmutableArray<EntityDefModel> Defs, ImmutableArray<UserEntityInfo> Users), ProcessContext Ctx) input)
    {
        var (defsAndUsers, ctx) = input;
        var (defs, users) = defsAndUsers;

        if (defs.IsDefaultOrEmpty || users.IsDefaultOrEmpty)
            return;

        // Build def lookup
        var defMap = new Dictionary<string, EntityDefModel>();
        foreach (var def in defs)
        {
            if (!string.IsNullOrEmpty(def.Name))
                defMap[def.Name] = def;
        }

        // Build type index map (1-based, alphabetically sorted by entity name)
        var allNames = defs.Select(d => d.Name).OrderBy(n => n).ToList();
        var typeIndexMap = new Dictionary<string, ushort>();
        for (int i = 0; i < allNames.Count; i++)
            typeIndexMap[allNames[i]] = (ushort)(i + 1);

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
            var properties = PropertiesEmitter.Emit(def, user.ClassName, user.Namespace, ctx);
            if (properties != null)
                spc.AddSource($"{user.ClassName}.Properties.g.cs", SourceText.From(properties, System.Text.Encoding.UTF8));

            // Serialization (Serialize/Deserialize + TypeName)
            var serialization = SerializationEmitter.Emit(def, user.ClassName, user.Namespace, ctx);
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
            var dispatcher = DispatcherEmitter.Emit(entityList, ctx, typeIndexMap, dispatchBase);
            spc.AddSource("DefRpcDispatcher.g.cs", SourceText.From(dispatcher, System.Text.Encoding.UTF8));
        }

        // Global: TypeRegistry — serializes entity descriptors (with RPC + ExposedScope) to C++
        if (entityList.Count > 0)
        {
            var typeRegistry = Emitters.TypeRegistryEmitter.Emit(entityList, typeIndexMap, ctx);
            spc.AddSource("DefEntityTypeRegistry.g.cs", SourceText.From(typeRegistry, System.Text.Encoding.UTF8));
        }
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
