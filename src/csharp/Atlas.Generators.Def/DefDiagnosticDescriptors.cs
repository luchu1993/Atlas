using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Def;

internal static class DefDiagnosticDescriptors
{
    public static readonly DiagnosticDescriptor DEF001 = new(
        "ATLAS_DEF001",
        "[Entity] type has no matching .def file",
        "[Entity(\"{0}\")] has no matching .def file",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF002 = new(
        "ATLAS_DEF002",
        "client_methods must not have exposed attribute",
        "client_methods.{0}: 'exposed' is not allowed on client methods (they are called by the server)",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF003 = new(
        "ATLAS_DEF003",
        "base_methods must not use exposed=\"all_clients\"",
        "base_methods.{0}: exposed=\"all_clients\" is not allowed on base methods (architecture constraint)",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF006 = new(
        "ATLAS_DEF006",
        ".def XML parse error",
        "Failed to parse .def file '{0}': {1}",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF007 = new(
        "ATLAS_DEF007",
        "Duplicate type_id",
        "Entity '{0}' has duplicate type_id {1}",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF008 = new(
        "ATLAS_DEF008",
        "'position' is reserved for the volatile channel and cannot be replicated as a property",
        "Entity '{0}': property 'position' with a replicable scope is reserved — "
        + "position is already transported by the volatile channel (kEntityPositionUpdate) "
        + "and the ClientEntity base class. The declaration will be skipped in generated "
        + "replication code to avoid double-delivery. Rename the property or remove it from the .def.",
        "Atlas.Def",
        DiagnosticSeverity.Warning,
        isEnabledByDefault: true);
}
