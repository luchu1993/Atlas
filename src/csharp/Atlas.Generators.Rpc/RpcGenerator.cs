using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using System.Threading;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Atlas.Generators.Rpc;

[Generator(LanguageNames.CSharp)]
public sealed class RpcGenerator : IIncrementalGenerator
{
    private const string EntityAttributeFullName = "Atlas.Entity.EntityAttribute";

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var entityClasses = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                EntityAttributeFullName,
                predicate: static (node, _) => node is ClassDeclarationSyntax,
                transform: static (ctx, ct) => ParseRpcEntity(ctx, ct))
            .Where(static m => m is not null)!;

        context.RegisterSourceOutput(
            entityClasses.Collect(),
            static (spc, models) => Execute(spc, models!));
    }

    private static RpcEntityModel? ParseRpcEntity(
        GeneratorAttributeSyntaxContext ctx, CancellationToken ct)
    {
        if (ctx.TargetSymbol is not INamedTypeSymbol classSymbol)
            return null;

        var entityAttr = classSymbol.GetAttributes()
            .FirstOrDefault(a => a.AttributeClass?.ToDisplayString() == EntityAttributeFullName);
        if (entityAttr is null) return null;

        var typeName = entityAttr.ConstructorArguments.Length > 0
            ? entityAttr.ConstructorArguments[0].Value?.ToString() ?? classSymbol.Name
            : classSymbol.Name;

        var model = new RpcEntityModel
        {
            Namespace = classSymbol.ContainingNamespace.IsGlobalNamespace
                ? ""
                : classSymbol.ContainingNamespace.ToDisplayString(),
            ClassName = classSymbol.Name,
            TypeName = typeName,
        };

        foreach (var member in classSymbol.GetMembers())
        {
            if (member is not IMethodSymbol method) continue;

            string? direction = null;
            bool reliable = true;

            foreach (var attr in method.GetAttributes())
            {
                var attrName = attr.AttributeClass?.ToDisplayString();
                if (attrName == "Atlas.Rpc.ClientRpcAttribute") direction = "Client";
                else if (attrName == "Atlas.Rpc.ServerRpcAttribute") direction = "Server";
                else if (attrName == "Atlas.Rpc.CellRpcAttribute") direction = "Cell";
                else if (attrName == "Atlas.Rpc.BaseRpcAttribute") direction = "Base";

                if (direction != null)
                {
                    foreach (var kvp in attr.NamedArguments)
                    {
                        if (kvp.Key == "Reliable" && kvp.Value.Value is bool val)
                            reliable = val;
                    }
                    break;
                }
            }

            if (direction == null) continue;

            var methodDecl = method.DeclaringSyntaxReferences
                .FirstOrDefault()?.GetSyntax(ct) as MethodDeclarationSyntax;

            var rpcMethod = new RpcMethodModel
            {
                MethodName = method.Name,
                IsPartial = methodDecl?.Modifiers.Any(SyntaxKind.PartialKeyword) ?? false,
                IsVoid = method.ReturnsVoid,
                Reliable = reliable,
            };

            foreach (var param in method.Parameters)
            {
                var typeDisplay = param.Type.ToDisplayString();
                var isSupported = RpcTypeHelper.TryGetSerializationMethods(
                    typeDisplay, out var wm, out var rm, out var dtb);

                rpcMethod.Parameters.Add(new RpcParamModel
                {
                    ParamName = param.Name,
                    TypeFullName = typeDisplay,
                    WriterMethod = wm,
                    ReaderMethod = rm,
                    IsSupported = isSupported,
                    DataTypeByte = dtb,
                });
            }

            switch (direction)
            {
                case "Client": model.ClientRpcs.Add(rpcMethod); break;
                case "Server": model.ServerRpcs.Add(rpcMethod); break;
                case "Cell": model.CellRpcs.Add(rpcMethod); break;
                case "Base": model.BaseRpcs.Add(rpcMethod); break;
            }
        }

        return model;
    }

    private static void Execute(
        SourceProductionContext spc,
        ImmutableArray<RpcEntityModel> models)
    {
        if (models.Length == 0) return;

        // Build type index over ALL [Entity] types, sorted by TypeName.
        // This must match EntityTypeRegistry's ordering so RPC IDs use
        // the same type index as the C++ EntityDefRegistry.
        var allSorted = models.OrderBy(m => m.TypeName).ToList();

        var typeIndexMap = new Dictionary<string, ushort>();
        for (int i = 0; i < allSorted.Count; i++)
            typeIndexMap[allSorted[i].TypeName] = (ushort)(i + 1);

        // Only generate code for entities that actually have RPCs
        var withRpcs = allSorted.Where(m =>
            m.ClientRpcs.Count > 0 || m.ServerRpcs.Count > 0 ||
            m.CellRpcs.Count > 0 || m.BaseRpcs.Count > 0).ToList();

        if (withRpcs.Count == 0) return;

        foreach (var model in withRpcs)
        {
            ValidateModel(spc, model);

            var typeIndex = typeIndexMap[model.TypeName];
            var rpcIds = GenerateRpcIds(model, typeIndex);

            var sendStubSource = EmitSendStubs(model, rpcIds);
            spc.AddSource($"{model.ClassName}.RpcStubs.g.cs", sendStubSource);

            var mailboxSource = EmitMailboxProxies(model, rpcIds);
            if (mailboxSource != null)
                spc.AddSource($"{model.ClassName}.Mailbox.g.cs", mailboxSource);
        }

        var rpcIdsSource = EmitRpcIdConstants(withRpcs, typeIndexMap);
        spc.AddSource("RpcIds.g.cs", rpcIdsSource);

        var dispatcherSource = EmitDispatcher(withRpcs, typeIndexMap);
        spc.AddSource("RpcDispatcher.g.cs", dispatcherSource);
    }

    private static void ValidateModel(SourceProductionContext spc, RpcEntityModel model)
    {
        var allMethods = model.ClientRpcs
            .Concat(model.ServerRpcs)
            .Concat(model.CellRpcs)
            .Concat(model.BaseRpcs);

        foreach (var method in allMethods)
        {
            if (!method.IsPartial)
            {
                spc.ReportDiagnostic(Diagnostic.Create(
                    RpcDiagnosticDescriptors.MethodNotPartial,
                    Location.None, method.MethodName));
            }
            if (!method.IsVoid)
            {
                spc.ReportDiagnostic(Diagnostic.Create(
                    RpcDiagnosticDescriptors.NonVoidReturn,
                    Location.None, method.MethodName));
            }
            foreach (var param in method.Parameters)
            {
                if (!param.IsSupported)
                {
                    spc.ReportDiagnostic(Diagnostic.Create(
                        RpcDiagnosticDescriptors.UnsupportedParameterType,
                        Location.None, param.ParamName, method.MethodName, param.TypeFullName));
                }
            }
        }
    }

    private static Dictionary<string, int> GenerateRpcIds(RpcEntityModel model, ushort typeIndex)
    {
        var ids = new Dictionary<string, int>();
        AssignIds(ids, model.ClassName, model.ClientRpcs, typeIndex, 0x00);
        AssignIds(ids, model.ClassName, model.ServerRpcs, typeIndex, 0x01);
        AssignIds(ids, model.ClassName, model.CellRpcs, typeIndex, 0x02);
        AssignIds(ids, model.ClassName, model.BaseRpcs, typeIndex, 0x03);
        return ids;
    }

    /// <summary>
    /// Packed RPC ID encoding: [direction:2 | typeIndex:14 | method:8] = 24 bits.
    /// rpcId = (direction &lt;&lt; 22) | (typeIndex &lt;&lt; 8) | method
    /// </summary>
    private static void AssignIds(
        Dictionary<string, int> ids, string className,
        List<RpcMethodModel> methods, ushort typeIndex, byte direction)
    {
        var sorted = methods.OrderBy(m => m.MethodName).ToList();
        for (int i = 0; i < sorted.Count; i++)
        {
            int id = (direction << 22) | (typeIndex << 8) | (i + 1);
            ids[$"{className}_{sorted[i].MethodName}"] = id;
        }
    }

    private static string EmitSendStubs(RpcEntityModel model, Dictionary<string, int> rpcIds)
    {
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated by Atlas.Generators.Rpc/>");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("using System;");
        sb.AppendLine("using Atlas.Serialization;");
        sb.AppendLine("using Atlas.Rpc;");
        sb.AppendLine();

        if (!string.IsNullOrEmpty(model.Namespace))
        {
            sb.AppendLine($"namespace {model.Namespace};");
            sb.AppendLine();
        }

        sb.AppendLine($"public partial class {model.ClassName}");
        sb.AppendLine("{");

        foreach (var rpc in model.ServerRpcs)
            EmitServerRpcStub(sb, model.ClassName, rpc, rpcIds);
        foreach (var rpc in model.ClientRpcs)
            EmitSendStub(sb, model.ClassName, rpc, "Client", rpcIds);
        foreach (var rpc in model.CellRpcs)
            EmitSendStub(sb, model.ClassName, rpc, "Cell", rpcIds);
        foreach (var rpc in model.BaseRpcs)
            EmitSendStub(sb, model.ClassName, rpc, "Base", rpcIds);

        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitServerRpcStub(
        StringBuilder sb, string className, RpcMethodModel rpc,
        Dictionary<string, int> rpcIds)
    {
        var paramList = string.Join(", ", rpc.Parameters.Select(p => $"{p.TypeFullName} {p.ParamName}"));
        sb.AppendLine($"    public partial void {rpc.MethodName}({paramList})");
        sb.AppendLine("    {");
        sb.AppendLine($"        throw new System.InvalidOperationException(");
        sb.AppendLine($"            \"[ServerRpc] {rpc.MethodName} can only be sent from the client. \" +");
        sb.AppendLine($"            \"On the server, implement On{rpc.MethodName}() to handle the incoming RPC.\");");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitSendStub(
        StringBuilder sb, string className, RpcMethodModel rpc,
        string direction, Dictionary<string, int> rpcIds)
    {
        var paramList = string.Join(", ", rpc.Parameters.Select(p => $"{p.TypeFullName} {p.ParamName}"));
        var idKey = $"{className}_{rpc.MethodName}";
        var rpcIdExpr = $"RpcIds.{idKey}";
        var capacity = RpcTypeHelper.EstimatePayloadSize(rpc.Parameters);

        sb.AppendLine($"    public partial void {rpc.MethodName}({paramList})");
        sb.AppendLine("    {");

        if (rpc.Parameters.Count == 0)
        {
            switch (direction)
            {
                case "Client":
                    sb.AppendLine($"        this.SendClientRpc({rpcIdExpr}, ReadOnlySpan<byte>.Empty);");
                    break;
                case "Cell":
                    sb.AppendLine($"        this.SendCellRpc({rpcIdExpr}, ReadOnlySpan<byte>.Empty);");
                    break;
                case "Base":
                    sb.AppendLine($"        this.SendBaseRpc({rpcIdExpr}, ReadOnlySpan<byte>.Empty);");
                    break;
            }
        }
        else
        {
            sb.AppendLine($"        var writer = new SpanWriter({capacity});");
            sb.AppendLine("        try");
            sb.AppendLine("        {");
            foreach (var param in rpc.Parameters)
            {
                sb.AppendLine($"            writer.{param.WriterMethod}({param.ParamName});");
            }
            switch (direction)
            {
                case "Client":
                    sb.AppendLine($"            this.SendClientRpc({rpcIdExpr}, writer.WrittenSpan);");
                    break;
                case "Cell":
                    sb.AppendLine($"            this.SendCellRpc({rpcIdExpr}, writer.WrittenSpan);");
                    break;
                case "Base":
                    sb.AppendLine($"            this.SendBaseRpc({rpcIdExpr}, writer.WrittenSpan);");
                    break;
            }
            sb.AppendLine("        }");
            sb.AppendLine("        finally");
            sb.AppendLine("        {");
            sb.AppendLine("            writer.Dispose();");
            sb.AppendLine("        }");
        }

        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static string? EmitMailboxProxies(RpcEntityModel model, Dictionary<string, int> rpcIds)
    {
        bool hasClient = model.ClientRpcs.Count > 0;
        bool hasCell = model.CellRpcs.Count > 0;
        bool hasBase = model.BaseRpcs.Count > 0;

        if (!hasClient && !hasCell && !hasBase) return null;

        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated by Atlas.Generators.Rpc/>");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("using System;");
        sb.AppendLine("using Atlas.Serialization;");
        sb.AppendLine("using Atlas.Rpc;");
        sb.AppendLine();

        if (!string.IsNullOrEmpty(model.Namespace))
        {
            sb.AppendLine($"namespace {model.Namespace};");
            sb.AppendLine();
        }

        if (hasClient)
            EmitClientMailboxStruct(sb, model, rpcIds);

        if (hasCell)
            EmitDirectionMailboxStruct(sb, model, rpcIds, "Cell", model.CellRpcs);

        if (hasBase)
            EmitDirectionMailboxStruct(sb, model, rpcIds, "Base", model.BaseRpcs);

        // Entity properties for mailbox access
        sb.AppendLine($"public partial class {model.ClassName}");
        sb.AppendLine("{");

        if (hasClient)
        {
            sb.AppendLine($"    public {model.ClassName}ClientMailbox Client => new(this, MailboxTarget.OwnerClient);");
            sb.AppendLine($"    public {model.ClassName}ClientMailbox AllClients => new(this, MailboxTarget.AllClients);");
            sb.AppendLine($"    public {model.ClassName}ClientMailbox OtherClients => new(this, MailboxTarget.OtherClients);");
        }
        if (hasCell)
            sb.AppendLine($"    public {model.ClassName}CellMailbox Cell => new(this);");
        if (hasBase)
            sb.AppendLine($"    public {model.ClassName}BaseMailbox Base => new(this);");

        sb.AppendLine("}");

        return sb.ToString();
    }

    private static void EmitClientMailboxStruct(
        StringBuilder sb, RpcEntityModel model, Dictionary<string, int> rpcIds)
    {
        sb.AppendLine($"public readonly struct {model.ClassName}ClientMailbox");
        sb.AppendLine("{");
        sb.AppendLine($"    private readonly {model.ClassName} _entity;");
        sb.AppendLine("    private readonly MailboxTarget _target;");
        sb.AppendLine();
        sb.AppendLine($"    internal {model.ClassName}ClientMailbox({model.ClassName} entity, MailboxTarget target)");
        sb.AppendLine("    {");
        sb.AppendLine("        _entity = entity;");
        sb.AppendLine("        _target = target;");
        sb.AppendLine("    }");

        foreach (var rpc in model.ClientRpcs)
        {
            var paramList = string.Join(", ", rpc.Parameters.Select(p => $"{p.TypeFullName} {p.ParamName}"));
            var idKey = $"{model.ClassName}_{rpc.MethodName}";
            var capacity = RpcTypeHelper.EstimatePayloadSize(rpc.Parameters);

            sb.AppendLine();
            sb.AppendLine($"    public void {rpc.MethodName}({paramList})");
            sb.AppendLine("    {");

            if (rpc.Parameters.Count == 0)
            {
                sb.AppendLine($"        _entity.SendClientRpc(RpcIds.{idKey}, ReadOnlySpan<byte>.Empty);");
            }
            else
            {
                sb.AppendLine($"        var writer = new SpanWriter({capacity});");
                sb.AppendLine("        try");
                sb.AppendLine("        {");
                foreach (var param in rpc.Parameters)
                    sb.AppendLine($"            writer.{param.WriterMethod}({param.ParamName});");
                sb.AppendLine($"            _entity.SendClientRpc(RpcIds.{idKey}, writer.WrittenSpan);");
                sb.AppendLine("        }");
                sb.AppendLine("        finally { writer.Dispose(); }");
            }

            sb.AppendLine("    }");
        }

        sb.AppendLine("}");
        sb.AppendLine();
    }

    private static void EmitDirectionMailboxStruct(
        StringBuilder sb, RpcEntityModel model, Dictionary<string, int> rpcIds,
        string direction, List<RpcMethodModel> rpcs)
    {
        var sendMethod = $"Send{direction}Rpc";

        sb.AppendLine($"public readonly struct {model.ClassName}{direction}Mailbox");
        sb.AppendLine("{");
        sb.AppendLine($"    private readonly {model.ClassName} _entity;");
        sb.AppendLine($"    internal {model.ClassName}{direction}Mailbox({model.ClassName} entity) => _entity = entity;");

        foreach (var rpc in rpcs)
        {
            var paramList = string.Join(", ", rpc.Parameters.Select(p => $"{p.TypeFullName} {p.ParamName}"));
            var idKey = $"{model.ClassName}_{rpc.MethodName}";
            var capacity = RpcTypeHelper.EstimatePayloadSize(rpc.Parameters);

            sb.AppendLine();
            sb.AppendLine($"    public void {rpc.MethodName}({paramList})");
            sb.AppendLine("    {");

            if (rpc.Parameters.Count == 0)
            {
                sb.AppendLine($"        _entity.{sendMethod}(RpcIds.{idKey}, ReadOnlySpan<byte>.Empty);");
            }
            else
            {
                sb.AppendLine($"        var writer = new SpanWriter({capacity});");
                sb.AppendLine("        try");
                sb.AppendLine("        {");
                foreach (var param in rpc.Parameters)
                    sb.AppendLine($"            writer.{param.WriterMethod}({param.ParamName});");
                sb.AppendLine($"            _entity.{sendMethod}(RpcIds.{idKey}, writer.WrittenSpan);");
                sb.AppendLine("        }");
                sb.AppendLine("        finally { writer.Dispose(); }");
            }

            sb.AppendLine("    }");
        }

        sb.AppendLine("}");
        sb.AppendLine();
    }

    private static string EmitRpcIdConstants(
        List<RpcEntityModel> sorted, Dictionary<string, ushort> typeIndexMap)
    {
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated by Atlas.Generators.Rpc/>");
        sb.AppendLine();
        sb.AppendLine("namespace Atlas.Rpc;");
        sb.AppendLine();
        sb.AppendLine("internal static partial class RpcIds");
        sb.AppendLine("{");

        foreach (var model in sorted)
        {
            var typeIndex = typeIndexMap[model.TypeName];
            EmitRpcIdGroup(sb, model.ClassName, model.ClientRpcs, typeIndex, 0x00, "ClientRpc");
            EmitRpcIdGroup(sb, model.ClassName, model.ServerRpcs, typeIndex, 0x01, "ServerRpc");
            EmitRpcIdGroup(sb, model.ClassName, model.CellRpcs, typeIndex, 0x02, "CellRpc");
            EmitRpcIdGroup(sb, model.ClassName, model.BaseRpcs, typeIndex, 0x03, "BaseRpc");
        }

        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitRpcIdGroup(
        StringBuilder sb, string className, List<RpcMethodModel> methods,
        ushort typeIndex, byte direction, string comment)
    {
        if (methods.Count == 0) return;

        sb.AppendLine($"    // {comment}");
        var sortedMethods = methods.OrderBy(m => m.MethodName).ToList();
        for (int i = 0; i < sortedMethods.Count; i++)
        {
            int id = (direction << 22) | (typeIndex << 8) | (i + 1);
            sb.AppendLine($"    public const int {className}_{sortedMethods[i].MethodName} = 0x{id:X6};");
        }
    }

    private static string EmitDispatcher(
        List<RpcEntityModel> sorted, Dictionary<string, ushort> typeIndexMap)
    {
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated by Atlas.Generators.Rpc/>");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("using Atlas.Serialization;");
        sb.AppendLine("using Atlas.Entity;");
        sb.AppendLine();
        sb.AppendLine("namespace Atlas.Rpc;");
        sb.AppendLine();
        sb.AppendLine("internal static partial class RpcDispatcher");
        sb.AppendLine("{");

        // Top-level dispatch for ServerRpc (received by server from client)
        sb.AppendLine("    public static void DispatchServerRpc(ServerEntity target, int rpcId, ref SpanReader reader)");
        sb.AppendLine("    {");
        sb.AppendLine("        switch (target)");
        sb.AppendLine("        {");
        foreach (var model in sorted)
        {
            if (model.ServerRpcs.Count == 0) continue;
            var fullName = string.IsNullOrEmpty(model.Namespace)
                ? model.ClassName
                : $"{model.Namespace}.{model.ClassName}";
            sb.AppendLine($"            case {fullName} t: Dispatch_{model.ClassName}_ServerRpc(t, rpcId, ref reader); break;");
        }
        sb.AppendLine("        }");
        sb.AppendLine("    }");
        sb.AppendLine();

        // Top-level dispatch for ClientRpc (received by client from server)
        sb.AppendLine("    public static void DispatchClientRpc(ServerEntity target, int rpcId, ref SpanReader reader)");
        sb.AppendLine("    {");
        sb.AppendLine("        switch (target)");
        sb.AppendLine("        {");
        foreach (var model in sorted)
        {
            if (model.ClientRpcs.Count == 0) continue;
            var fullName = string.IsNullOrEmpty(model.Namespace)
                ? model.ClassName
                : $"{model.Namespace}.{model.ClassName}";
            sb.AppendLine($"            case {fullName} t: Dispatch_{model.ClassName}_ClientRpc(t, rpcId, ref reader); break;");
        }
        sb.AppendLine("        }");
        sb.AppendLine("    }");
        sb.AppendLine();

        // Top-level dispatch for CellRpc
        sb.AppendLine("    public static void DispatchCellRpc(ServerEntity target, int rpcId, ref SpanReader reader)");
        sb.AppendLine("    {");
        sb.AppendLine("        switch (target)");
        sb.AppendLine("        {");
        foreach (var model in sorted)
        {
            if (model.CellRpcs.Count == 0) continue;
            var fullName = string.IsNullOrEmpty(model.Namespace)
                ? model.ClassName
                : $"{model.Namespace}.{model.ClassName}";
            sb.AppendLine($"            case {fullName} t: Dispatch_{model.ClassName}_CellRpc(t, rpcId, ref reader); break;");
        }
        sb.AppendLine("        }");
        sb.AppendLine("    }");
        sb.AppendLine();

        // Top-level dispatch for BaseRpc
        sb.AppendLine("    public static void DispatchBaseRpc(ServerEntity target, int rpcId, ref SpanReader reader)");
        sb.AppendLine("    {");
        sb.AppendLine("        switch (target)");
        sb.AppendLine("        {");
        foreach (var model in sorted)
        {
            if (model.BaseRpcs.Count == 0) continue;
            var fullName = string.IsNullOrEmpty(model.Namespace)
                ? model.ClassName
                : $"{model.Namespace}.{model.ClassName}";
            sb.AppendLine($"            case {fullName} t: Dispatch_{model.ClassName}_BaseRpc(t, rpcId, ref reader); break;");
        }
        sb.AppendLine("        }");
        sb.AppendLine("    }");

        // Per-entity dispatch methods
        foreach (var model in sorted)
        {
            var typeIndex = typeIndexMap[model.TypeName];
            var fullName = string.IsNullOrEmpty(model.Namespace)
                ? model.ClassName
                : $"{model.Namespace}.{model.ClassName}";

            EmitEntityDispatch(sb, fullName, model.ClassName, model.ServerRpcs, typeIndex, 0x01, "ServerRpc");
            EmitEntityDispatch(sb, fullName, model.ClassName, model.ClientRpcs, typeIndex, 0x00, "ClientRpc");
            EmitEntityDispatch(sb, fullName, model.ClassName, model.CellRpcs, typeIndex, 0x02, "CellRpc");
            EmitEntityDispatch(sb, fullName, model.ClassName, model.BaseRpcs, typeIndex, 0x03, "BaseRpc");
        }

        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitEntityDispatch(
        StringBuilder sb, string fullName, string className,
        List<RpcMethodModel> methods, ushort typeIndex, byte direction, string label)
    {
        if (methods.Count == 0) return;

        sb.AppendLine();
        sb.AppendLine($"    private static void Dispatch_{className}_{label}({fullName} target, int rpcId, ref SpanReader reader)");
        sb.AppendLine("    {");
        sb.AppendLine("        switch (rpcId)");
        sb.AppendLine("        {");

        var sortedMethods = methods.OrderBy(m => m.MethodName).ToList();
        for (int i = 0; i < sortedMethods.Count; i++)
        {
            var method = sortedMethods[i];
            int id = (direction << 22) | (typeIndex << 8) | (i + 1);
            sb.AppendLine($"            case 0x{id:X6}:");
            sb.AppendLine("            {");
            foreach (var param in method.Parameters)
            {
                sb.AppendLine($"                var {param.ParamName} = reader.{param.ReaderMethod}();");
            }
            var args = string.Join(", ", method.Parameters.Select(p => p.ParamName));
            sb.AppendLine($"                target.On{method.MethodName}({args});");
            sb.AppendLine("                break;");
            sb.AppendLine("            }");
        }

        sb.AppendLine("        }");
        sb.AppendLine("    }");
    }
}
