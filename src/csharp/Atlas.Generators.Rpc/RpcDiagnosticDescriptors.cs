using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Rpc;

internal static class RpcDiagnosticDescriptors
{
    public static readonly DiagnosticDescriptor MethodNotPartial = new(
        id: "ATLAS_RPC001",
        title: "RPC method must be partial",
        messageFormat: "Method '{0}' is annotated with an RPC attribute but is not declared as partial",
        category: "Atlas.Rpc",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor UnsupportedParameterType = new(
        id: "ATLAS_RPC002",
        title: "Unsupported RPC parameter type",
        messageFormat: "Parameter '{0}' of method '{1}' has unsupported type '{2}'",
        category: "Atlas.Rpc",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public static readonly DiagnosticDescriptor NonVoidReturn = new(
        id: "ATLAS_RPC003",
        title: "RPC method must return void",
        messageFormat: "RPC method '{0}' must have a void return type",
        category: "Atlas.Rpc",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true);
}
