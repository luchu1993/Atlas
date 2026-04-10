using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Interop;

internal static class DiagnosticDescriptors
{
    private const string Category = "Atlas.Interop";

    public static readonly DiagnosticDescriptor ClassNotPartial = new(
        id: "ATLAS_INTEROP001",
        title: "Class must be partial",
        messageFormat: "Class '{0}' must be declared partial to use [NativeImport]",
        category: Category,
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor MethodNotPartial = new(
        id: "ATLAS_INTEROP002",
        title: "Method must be partial",
        messageFormat: "Method '{0}' must be declared partial to use [NativeImport]",
        category: Category,
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor UnsupportedParameterType = new(
        id: "ATLAS_INTEROP003",
        title: "Unsupported parameter type",
        messageFormat:
            "Parameter '{0}' of method '{1}' has unsupported type '{2}'. " +
            "Supported: blittable primitives, IntPtr, nint, nuint, ReadOnlySpan<byte>, string, unmanaged structs.",
        category: Category,
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor DuplicateEntryPoint = new(
        id: "ATLAS_INTEROP004",
        title: "Duplicate entryPoint",
        messageFormat: "EntryPoint '{0}' on method '{1}' was already used on method '{2}'",
        category: Category,
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true);
}
