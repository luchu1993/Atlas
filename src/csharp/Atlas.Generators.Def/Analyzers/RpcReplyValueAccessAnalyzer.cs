using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Atlas.Generators.Def.Analyzers;

// Heuristic: warns when r.Value is reached and the enclosing method body
// has no IsOk / IsBusinessError / IsFrameworkError / TryGetValue check on
// the same RpcReply<T> receiver. Coarse but stops the most common foot-
// gun without dataflow analysis.
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class RpcReplyValueAccessAnalyzer : DiagnosticAnalyzer
{
    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics =>
        ImmutableArray.Create(DefDiagnosticDescriptors.RPC001);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterSyntaxNodeAction(Analyze, SyntaxKind.SimpleMemberAccessExpression);
    }

    private static readonly string[] kGuardMembers =
    {
        "IsOk", "IsBusinessError", "IsFrameworkError", "TryGetValue",
    };

    private static void Analyze(SyntaxNodeAnalysisContext ctx)
    {
        var ma = (MemberAccessExpressionSyntax)ctx.Node;
        if (ma.Name.Identifier.Text != "Value") return;

        var typeInfo = ctx.SemanticModel.GetTypeInfo(ma.Expression);
        if (typeInfo.Type is not INamedTypeSymbol nt) return;
        if (nt.Name != "RpcReply") return;
        if (nt.ContainingNamespace?.ToDisplayString() != "Atlas.Coro.Rpc") return;

        SyntaxNode? enclosing = ma.FirstAncestorOrSelf<MethodDeclarationSyntax>();
        enclosing ??= ma.FirstAncestorOrSelf<LocalFunctionStatementSyntax>();
        enclosing ??= ma.FirstAncestorOrSelf<AnonymousFunctionExpressionSyntax>();
        if (enclosing is null) return;

        var receiver = ctx.SemanticModel.GetSymbolInfo(ma.Expression).Symbol;
        if (receiver is null) return;

        bool guarded = enclosing.DescendantNodes()
            .OfType<MemberAccessExpressionSyntax>()
            .Where(o => o != ma)
            .Where(o => kGuardMembers.Contains(o.Name.Identifier.Text))
            .Any(o => SymbolEqualityComparer.Default.Equals(
                ctx.SemanticModel.GetSymbolInfo(o.Expression).Symbol, receiver));

        if (!guarded)
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                DefDiagnosticDescriptors.RPC001, ma.GetLocation(), receiver.Name));
        }
    }
}
