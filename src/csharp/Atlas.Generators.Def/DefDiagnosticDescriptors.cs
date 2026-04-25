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

    public static readonly DiagnosticDescriptor DEF009 = new(
        "ATLAS_DEF009",
        "Malformed type expression",
        "Type expression '{0}' is malformed: {1}",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF010 = new(
        "ATLAS_DEF010",
        "Type expression nested beyond depth limit",
        "Type expression '{0}' nests {1} levels deep; max allowed is {2}",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF011 = new(
        "ATLAS_DEF011",
        "dict key type must be scalar",
        "dict<K,V>: key type '{0}' must be a scalar (string / int{{8,16,32,64}} / uint{{8,16,32,64}})",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF012 = new(
        "ATLAS_DEF012",
        "Duplicate struct name in <types>",
        "Struct '{0}' declared more than once in entity '{1}' <types> section",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF013 = new(
        "ATLAS_DEF013",
        "Struct has cyclic reference",
        "Struct '{0}' participates in a reference cycle: {1}",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    // Info-level diagnostic: the emitter surfaces its auto-sync choice
    // per struct so developers can see how their declarations will be
    // serialised without reading generator source.
    public static readonly DiagnosticDescriptor DEF014 = new(
        "ATLAS_DEF014",
        "Struct sync strategy",
        "struct '{0}' → {1} ({2})",
        "Atlas.Def",
        DiagnosticSeverity.Info,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF015 = new(
        "ATLAS_DEF015",
        "Duplicate property name",
        "Entity '{0}' declares property '{1}' more than once",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF016 = new(
        "ATLAS_DEF016",
        "Struct/alias name collides across entities",
        "Name '{0}' is declared as a struct and as an alias (in entity '{1}'); "
            + "a name must be either a struct or an alias, not both",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DEF017 = new(
        "ATLAS_DEF017",
        "Alias reference chain too deep or cyclic",
        "Alias '{0}' forms a chain longer than {1} links — either a cycle (A → B → A) "
            + "or an unreasonably deep alias-of-alias reference",
        "Atlas.Def",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);
}
