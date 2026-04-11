using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Events;

internal static class EventDiagnosticDescriptors
{
    public static readonly DiagnosticDescriptor MissingEventListener = new(
        id: "ATLAS_EVENT001",
        title: "Class must implement IEventListener",
        messageFormat: "Class '{0}' has [EventHandler] methods but does not implement IEventListener",
        category: "Atlas.Events",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor UnsupportedParameterType = new(
        id: "ATLAS_EVENT002",
        title: "Unsupported event handler parameter type",
        messageFormat: "Parameter '{0}' of handler '{1}' has unsupported type '{2}'",
        category: "Atlas.Events",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);
}
