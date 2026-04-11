using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Entity;

internal static class DiagnosticDescriptors
{
    public static readonly DiagnosticDescriptor NonPartialClass = new(
        id: "ATLAS_ENTITY001",
        title: "Entity class must be partial",
        messageFormat: "Class '{0}' is annotated with [Entity] but is not declared as partial",
        category: "Atlas.Entity",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor UnsupportedFieldType = new(
        id: "ATLAS_ENTITY002",
        title: "Unsupported entity field type",
        messageFormat: "Field '{0}' has unsupported type '{1}' for serialization",
        category: "Atlas.Entity",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor MissingBaseClass = new(
        id: "ATLAS_ENTITY003",
        title: "Entity class must inherit ServerEntity",
        messageFormat: "Class '{0}' is annotated with [Entity] but does not inherit from ServerEntity",
        category: "Atlas.Entity",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor FieldNotPrivate = new(
        id: "ATLAS_ENTITY004",
        title: "Annotated field should be private",
        messageFormat: "Field '{0}' is not private; consider using a private field with _ prefix",
        category: "Atlas.Entity",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor FieldMissingUnderscore = new(
        id: "ATLAS_ENTITY005",
        title: "Field name should start with underscore",
        messageFormat: "Field '{0}' does not start with '_'; cannot derive property name",
        category: "Atlas.Entity",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true);
}
