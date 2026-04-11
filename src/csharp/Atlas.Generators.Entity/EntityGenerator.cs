using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Threading;
using Atlas.Generators.Entity.Emitters;
using Atlas.Generators.Entity.Model;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Atlas.Generators.Entity;

[Generator(LanguageNames.CSharp)]
public sealed class EntityGenerator : IIncrementalGenerator
{
    private const string EntityAttributeFullName = "Atlas.Entity.EntityAttribute";
    private const string ReplicatedAttributeFullName = "Atlas.Entity.ReplicatedAttribute";
    private const string PersistentAttributeFullName = "Atlas.Entity.PersistentAttribute";
    private const string ServerOnlyAttributeFullName = "Atlas.Entity.ServerOnlyAttribute";

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var entityClasses = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                EntityAttributeFullName,
                predicate: static (node, _) => node is ClassDeclarationSyntax,
                transform: static (ctx, ct) => ParseEntityModel(ctx, ct))
            .Where(static m => m is not null)!;

        context.RegisterSourceOutput(
            entityClasses.Collect(),
            static (spc, models) => Execute(spc, models!));
    }

    private static EntityModel? ParseEntityModel(
        GeneratorAttributeSyntaxContext ctx, CancellationToken ct)
    {
        if (ctx.TargetSymbol is not INamedTypeSymbol classSymbol)
            return null;

        var classDecl = (ClassDeclarationSyntax)ctx.TargetNode;

        var entityAttr = classSymbol.GetAttributes()
            .FirstOrDefault(a => a.AttributeClass?.ToDisplayString() == EntityAttributeFullName);
        if (entityAttr is null) return null;

        var typeName = entityAttr.ConstructorArguments.Length > 0
            ? entityAttr.ConstructorArguments[0].Value?.ToString() ?? classSymbol.Name
            : classSymbol.Name;

        byte compression = 0;
        foreach (var kvp in entityAttr.NamedArguments)
        {
            if (kvp.Key == "Compression" && kvp.Value.Value != null)
                compression = System.Convert.ToByte(kvp.Value.Value);
        }

        var model = new EntityModel
        {
            Namespace = classSymbol.ContainingNamespace.IsGlobalNamespace
                ? ""
                : classSymbol.ContainingNamespace.ToDisplayString(),
            ClassName = classSymbol.Name,
            TypeName = typeName,
            IsPartial = classDecl.Modifiers.Any(SyntaxKind.PartialKeyword),
            InheritsServerEntity = InheritsFrom(classSymbol, "Atlas.Entity.ServerEntity"),
            Compression = compression,
        };

        foreach (var member in classSymbol.GetMembers())
        {
            if (member is not IFieldSymbol field) continue;
            if (field.IsStatic || field.IsConst) continue;

            var hasReplicated = HasAttribute(field, ReplicatedAttributeFullName);
            var hasPersistent = HasAttribute(field, PersistentAttributeFullName);
            var hasServerOnly = HasAttribute(field, ServerOnlyAttributeFullName);

            if (!hasReplicated && !hasPersistent && !hasServerOnly) continue;

            var typeDisplay = field.Type.ToDisplayString();
            var isSupported = TypeHelper.TryGetSerializationMethods(
                typeDisplay, out var writerMethod, out var readerMethod, out var dataTypeByte);

            byte replicationScope = 3; // AllClients default
            byte detailLevel = 5;     // sent to all AoI clients default
            if (hasReplicated)
            {
                var repAttr = field.GetAttributes()
                    .FirstOrDefault(a => a.AttributeClass?.ToDisplayString() == ReplicatedAttributeFullName);
                if (repAttr != null)
                {
                    foreach (var kvp in repAttr.NamedArguments)
                    {
                        if (kvp.Key == "Scope" && kvp.Value.Value != null)
                            replicationScope = System.Convert.ToByte(kvp.Value.Value);
                        else if (kvp.Key == "DetailLevel" && kvp.Value.Value != null)
                            detailLevel = System.Convert.ToByte(kvp.Value.Value);
                    }
                }
            }

            var fieldName = field.Name;
            model.Fields.Add(new FieldModel
            {
                FieldName = fieldName,
                PropertyName = TypeHelper.FieldNameToPropertyName(fieldName),
                TypeFullName = typeDisplay,
                WriterMethod = writerMethod,
                ReaderMethod = readerMethod,
                IsReplicated = hasReplicated,
                ReplicationScope = replicationScope,
                IsPersistent = hasPersistent,
                IsServerOnly = hasServerOnly,
                IsPrivate = field.DeclaredAccessibility == Accessibility.Private,
                HasUnderscorePrefix = fieldName.StartsWith("_"),
                IsSupportedType = isSupported,
                DataTypeByte = dataTypeByte,
                DetailLevel = detailLevel,
            });
        }

        return model;
    }

    private static void Execute(
        SourceProductionContext spc,
        ImmutableArray<EntityModel> models)
    {
        var validModels = new List<EntityModel>();

        foreach (var model in models)
        {
            if (!model.IsPartial)
            {
                spc.ReportDiagnostic(Diagnostic.Create(
                    DiagnosticDescriptors.NonPartialClass,
                    Location.None, model.ClassName));
                continue;
            }

            if (!model.InheritsServerEntity)
            {
                spc.ReportDiagnostic(Diagnostic.Create(
                    DiagnosticDescriptors.MissingBaseClass,
                    Location.None, model.ClassName));
                continue;
            }

            foreach (var field in model.Fields)
            {
                if (!field.IsSupportedType)
                {
                    spc.ReportDiagnostic(Diagnostic.Create(
                        DiagnosticDescriptors.UnsupportedFieldType,
                        Location.None, field.FieldName, field.TypeFullName));
                }
                if (!field.IsPrivate)
                {
                    spc.ReportDiagnostic(Diagnostic.Create(
                        DiagnosticDescriptors.FieldNotPrivate,
                        Location.None, field.FieldName));
                }
                if (!field.HasUnderscorePrefix)
                {
                    spc.ReportDiagnostic(Diagnostic.Create(
                        DiagnosticDescriptors.FieldMissingUnderscore,
                        Location.None, field.FieldName));
                }
            }

            validModels.Add(model);

            var serializationSource = SerializationEmitter.Emit(model);
            spc.AddSource($"{model.ClassName}.Serialization.g.cs", serializationSource);

            var dirtyTrackingSource = DirtyTrackingEmitter.Emit(model);
            spc.AddSource($"{model.ClassName}.DirtyTracking.g.cs", dirtyTrackingSource);

            var deltaSyncSource = DeltaSyncEmitter.Emit(model);
            if (deltaSyncSource != null)
                spc.AddSource($"{model.ClassName}.DeltaSync.g.cs", deltaSyncSource);
        }

        var factorySource = FactoryEmitter.Emit(validModels);
        spc.AddSource("EntityFactory.g.cs", factorySource);

        var registrySource = TypeRegistryEmitter.Emit(validModels);
        spc.AddSource("EntityTypeRegistry.g.cs", registrySource);
    }

    private static bool InheritsFrom(INamedTypeSymbol symbol, string baseFullName)
    {
        var current = symbol.BaseType;
        while (current != null)
        {
            if (current.ToDisplayString() == baseFullName)
                return true;
            current = current.BaseType;
        }
        return false;
    }

    private static bool HasAttribute(ISymbol symbol, string attributeFullName)
    {
        return symbol.GetAttributes().Any(a =>
            a.AttributeClass?.ToDisplayString() == attributeFullName);
    }
}
