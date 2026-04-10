using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using System.Threading;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Atlas.Generators.Interop;

// ============================================================================
// Data model — records for incremental cache value equality
// ============================================================================

internal enum ParameterStrategy
{
    Blittable,
    SpanByte,
    String,
    Unsupported
}

internal sealed class ParameterInfo : IEquatable<ParameterInfo>
{
    public string Name { get; }
    public string TypeDisplayName { get; }
    public ParameterStrategy Strategy { get; }

    public ParameterInfo(string name, string typeDisplayName, ParameterStrategy strategy)
    {
        Name = name;
        TypeDisplayName = typeDisplayName;
        Strategy = strategy;
    }

    public bool Equals(ParameterInfo? other) =>
        other is not null &&
        Name == other.Name &&
        TypeDisplayName == other.TypeDisplayName &&
        Strategy == other.Strategy;

    public override bool Equals(object? obj) => Equals(obj as ParameterInfo);
    public override int GetHashCode() => (Name, TypeDisplayName, Strategy).GetHashCode();
}

internal sealed class NativeMethodInfo : IEquatable<NativeMethodInfo>
{
    public string ClassName { get; }
    public string ClassNamespace { get; }
    public string ClassAccessibility { get; }
    public string MethodName { get; }
    public string MethodAccessibility { get; }
    public string EntryPoint { get; }
    public string LibraryName { get; }
    public string ReturnType { get; }
    public EquatableArray<ParameterInfo> Parameters { get; }
    public bool NeedsWrapper { get; }

    // Not part of equality — used only for diagnostic locations.
    public Location? Location { get; }

    // Validation flags (not part of equality).
    public bool ClassIsPartial { get; }
    public bool MethodIsPartial { get; }
    public bool HasUnsupportedParams { get; }

    public NativeMethodInfo(
        string className, string classNamespace, string classAccessibility,
        string methodName, string methodAccessibility,
        string entryPoint, string libraryName,
        string returnType, EquatableArray<ParameterInfo> parameters,
        bool needsWrapper, Location? location,
        bool classIsPartial, bool methodIsPartial, bool hasUnsupportedParams)
    {
        ClassName = className;
        ClassNamespace = classNamespace;
        ClassAccessibility = classAccessibility;
        MethodName = methodName;
        MethodAccessibility = methodAccessibility;
        EntryPoint = entryPoint;
        LibraryName = libraryName;
        ReturnType = returnType;
        Parameters = parameters;
        NeedsWrapper = needsWrapper;
        Location = location;
        ClassIsPartial = classIsPartial;
        MethodIsPartial = methodIsPartial;
        HasUnsupportedParams = hasUnsupportedParams;
    }

    public bool Equals(NativeMethodInfo? other) =>
        other is not null &&
        ClassName == other.ClassName &&
        ClassNamespace == other.ClassNamespace &&
        MethodName == other.MethodName &&
        EntryPoint == other.EntryPoint &&
        LibraryName == other.LibraryName &&
        ReturnType == other.ReturnType &&
        Parameters.Equals(other.Parameters);

    public override bool Equals(object? obj) => Equals(obj as NativeMethodInfo);
    public override int GetHashCode() => (ClassName, ClassNamespace, MethodName, EntryPoint).GetHashCode();
}

// ============================================================================
// InteropGenerator — IIncrementalGenerator
// ============================================================================

[Generator]
public sealed class InteropGenerator : IIncrementalGenerator
{
    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        // 1. Inject [NativeImport] attribute into every compilation.
        context.RegisterPostInitializationOutput(static ctx =>
            ctx.AddSource(NativeImportAttributeSource.HintName,
                          NativeImportAttributeSource.Source));

        // 2. Find methods decorated with [NativeImport].
        var methods = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                NativeImportAttributeSource.FullyQualifiedName,
                predicate: static (node, _) => node is MethodDeclarationSyntax,
                transform: static (ctx, ct) => ExtractMethodInfo(ctx, ct))
            .Where(static m => m is not null)
            .Select(static (m, _) => m!);

        // 3. Collect all methods and emit source.
        var collected = methods.Collect();
        context.RegisterSourceOutput(collected, static (spc, methods) => Execute(spc, methods));
    }

    // ========================================================================
    // Transform: syntax + semantic → NativeMethodInfo
    // ========================================================================

    private static NativeMethodInfo? ExtractMethodInfo(
        GeneratorAttributeSyntaxContext ctx, CancellationToken ct)
    {
        if (ctx.TargetSymbol is not IMethodSymbol methodSymbol)
            return null;

        var containingType = methodSymbol.ContainingType;
        if (containingType is null)
            return null;

        // Read attribute arguments.
        var attrData = ctx.Attributes.FirstOrDefault();
        if (attrData is null)
            return null;

        string entryPoint = attrData.ConstructorArguments.Length > 0
            ? attrData.ConstructorArguments[0].Value as string ?? ""
            : "";

        string libraryName = "atlas_engine";
        foreach (var named in attrData.NamedArguments)
        {
            if (named.Key == "LibraryName" && named.Value.Value is string lib)
                libraryName = lib;
        }

        // Check partial modifiers.
        var methodSyntax = ctx.TargetNode as MethodDeclarationSyntax;
        bool methodIsPartial = methodSyntax?.Modifiers.Any(SyntaxKind.PartialKeyword) ?? false;

        bool classIsPartial = false;
        foreach (var typeDecl in containingType.DeclaringSyntaxReferences)
        {
            if (typeDecl.GetSyntax(ct) is TypeDeclarationSyntax tds &&
                tds.Modifiers.Any(SyntaxKind.PartialKeyword))
            {
                classIsPartial = true;
                break;
            }
        }

        // Classify parameters.
        var paramsBuilder = ImmutableArray.CreateBuilder<ParameterInfo>();
        bool needsWrapper = false;
        bool hasUnsupported = false;

        foreach (var param in methodSymbol.Parameters)
        {
            ct.ThrowIfCancellationRequested();
            var strategy = ClassifyType(param.Type);
            if (strategy == ParameterStrategy.SpanByte || strategy == ParameterStrategy.String)
                needsWrapper = true;
            if (strategy == ParameterStrategy.Unsupported)
                hasUnsupported = true;

            paramsBuilder.Add(new ParameterInfo(
                param.Name,
                param.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                strategy));
        }

        string returnType = methodSymbol.ReturnsVoid
            ? "void"
            : methodSymbol.ReturnType.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat);

        return new NativeMethodInfo(
            className: containingType.Name,
            classNamespace: containingType.ContainingNamespace?.ToDisplayString() ?? "",
            classAccessibility: AccessibilityToString(containingType.DeclaredAccessibility),
            methodName: methodSymbol.Name,
            methodAccessibility: AccessibilityToString(methodSymbol.DeclaredAccessibility),
            entryPoint: entryPoint,
            libraryName: libraryName,
            returnType: returnType,
            parameters: paramsBuilder.ToImmutable(),
            needsWrapper: needsWrapper,
            location: methodSyntax?.Identifier.GetLocation(),
            classIsPartial: classIsPartial,
            methodIsPartial: methodIsPartial,
            hasUnsupportedParams: hasUnsupported);
    }

    // ========================================================================
    // Type classification
    // ========================================================================

    private static ParameterStrategy ClassifyType(ITypeSymbol type)
    {
        // ReadOnlySpan<byte>
        if (type is INamedTypeSymbol named &&
            named.IsGenericType &&
            named.ConstructedFrom.ToDisplayString() == "System.ReadOnlySpan<T>" &&
            named.TypeArguments.Length == 1 &&
            named.TypeArguments[0].SpecialType == SpecialType.System_Byte)
        {
            return ParameterStrategy.SpanByte;
        }

        // string
        if (type.SpecialType == SpecialType.System_String)
            return ParameterStrategy.String;

        // Known blittable primitives
        switch (type.SpecialType)
        {
            case SpecialType.System_Byte:
            case SpecialType.System_SByte:
            case SpecialType.System_Int16:
            case SpecialType.System_UInt16:
            case SpecialType.System_Int32:
            case SpecialType.System_UInt32:
            case SpecialType.System_Int64:
            case SpecialType.System_UInt64:
            case SpecialType.System_Single:
            case SpecialType.System_Double:
            case SpecialType.System_IntPtr:
            case SpecialType.System_UIntPtr:
                return ParameterStrategy.Blittable;
        }

        // nint / nuint (may not have SpecialType on all Roslyn versions)
        var display = type.ToDisplayString();
        if (display is "nint" or "nuint")
            return ParameterStrategy.Blittable;

        // Unmanaged struct → blittable
        if (type.IsValueType && type.IsUnmanagedType)
            return ParameterStrategy.Blittable;

        return ParameterStrategy.Unsupported;
    }

    private static string AccessibilityToString(Accessibility a) => a switch
    {
        Accessibility.Public => "public",
        Accessibility.Internal => "internal",
        Accessibility.Private => "private",
        Accessibility.Protected => "protected",
        Accessibility.ProtectedOrInternal => "protected internal",
        Accessibility.ProtectedAndInternal => "private protected",
        _ => "internal"
    };

    // ========================================================================
    // Execute: diagnostics + code generation
    // ========================================================================

    private static void Execute(
        SourceProductionContext spc,
        ImmutableArray<NativeMethodInfo> methods)
    {
        if (methods.IsDefaultOrEmpty)
            return;

        // --- Diagnostics pass ---
        var entryPointMap = new Dictionary<string, string>(); // entryPoint → first method name

        foreach (var m in methods)
        {
            if (!m.ClassIsPartial)
                spc.ReportDiagnostic(Diagnostic.Create(
                    DiagnosticDescriptors.ClassNotPartial,
                    m.Location, m.ClassName));

            if (!m.MethodIsPartial)
                spc.ReportDiagnostic(Diagnostic.Create(
                    DiagnosticDescriptors.MethodNotPartial,
                    m.Location, m.MethodName));

            foreach (var p in m.Parameters)
            {
                if (p.Strategy == ParameterStrategy.Unsupported)
                    spc.ReportDiagnostic(Diagnostic.Create(
                        DiagnosticDescriptors.UnsupportedParameterType,
                        m.Location, p.Name, m.MethodName, p.TypeDisplayName));
            }

            if (entryPointMap.TryGetValue(m.EntryPoint, out var existing))
                spc.ReportDiagnostic(Diagnostic.Create(
                    DiagnosticDescriptors.DuplicateEntryPoint,
                    m.Location, m.EntryPoint, m.MethodName, existing));
            else
                entryPointMap[m.EntryPoint] = m.MethodName;
        }

        // --- Group by class and generate ---
        var groups = methods
            .Where(m => m.ClassIsPartial && m.MethodIsPartial && !m.HasUnsupportedParams)
            .GroupBy(m => (m.ClassNamespace, m.ClassName));

        foreach (var group in groups)
        {
            var first = group.First();
            var sb = new StringBuilder();

            sb.AppendLine("// <auto-generated by Atlas.Generators.Interop />");
            sb.AppendLine("#nullable enable");
            sb.AppendLine();
            sb.AppendLine("using System;");
            sb.AppendLine("using System.Runtime.InteropServices;");
            sb.AppendLine();

            if (!string.IsNullOrEmpty(first.ClassNamespace))
            {
                sb.Append("namespace ").Append(first.ClassNamespace).AppendLine(";");
                sb.AppendLine();
            }

            sb.Append(first.ClassAccessibility)
              .Append(" static unsafe partial class ")
              .AppendLine(first.ClassName);
            sb.AppendLine("{");

            bool firstMethod = true;
            foreach (var m in group)
            {
                if (!firstMethod)
                    sb.AppendLine();
                firstMethod = false;
                GenerateMethod(sb, m);
            }

            sb.AppendLine("}");

            spc.AddSource($"{first.ClassName}.NativeImport.g.cs", sb.ToString());
        }
    }

    // ========================================================================
    // Per-method code generation
    // ========================================================================

    private static void GenerateMethod(StringBuilder sb, NativeMethodInfo m)
    {
        // All methods get: private [LibraryImport] _Method + public wrapper body.
        // For blittable-only methods the wrapper is a trivial forwarding call
        // (the JIT inlines it).  This is necessary because the user's partial
        // declaration is the "defining" half; the generator must provide the
        // "implementing" half (a body).  We cannot emit a second bodiless partial
        // — that would be two defining declarations (CS0756).
        GeneratePrivatePInvoke(sb, m);
        sb.AppendLine();
        GeneratePublicWrapper(sb, m);
    }

    private static void GeneratePrivatePInvoke(StringBuilder sb, NativeMethodInfo m)
    {
        // Use [DllImport] + static extern (not [LibraryImport] + partial) because
        // source generator output is NOT processed by other source generators.
        // For all-blittable parameters [DllImport] has zero marshaling overhead
        // and is IL2CPP-safe.
        sb.Append("    [global::System.Runtime.InteropServices.DllImport(\"")
          .Append(m.LibraryName)
          .Append("\", EntryPoint = \"").Append(m.EntryPoint)
          .AppendLine("\", ExactSpelling = true)]");
        sb.Append("    private static extern ").Append(m.ReturnType)
          .Append(" _").Append(m.MethodName).Append('(');

        bool first = true;
        foreach (var p in m.Parameters)
        {
            if (!first) sb.Append(", ");
            first = false;

            switch (p.Strategy)
            {
                case ParameterStrategy.SpanByte:
                case ParameterStrategy.String:
                    sb.Append("byte* ").Append(p.Name).Append(", int ").Append(p.Name).Append("Len");
                    break;
                default:
                    sb.Append(MinimalTypeName(p.TypeDisplayName)).Append(' ').Append(p.Name);
                    break;
            }
        }

        sb.AppendLine(");");
    }

    private static void GeneratePublicWrapper(StringBuilder sb, NativeMethodInfo m)
    {
        sb.Append("    ").Append(m.MethodAccessibility)
          .Append(" static partial ").Append(m.ReturnType)
          .Append(' ').Append(m.MethodName).Append('(');
        AppendOriginalParams(sb, m.Parameters);
        sb.AppendLine(")");
        sb.AppendLine("    {");

        bool hasReturn = m.ReturnType != "void";

        if (!m.NeedsWrapper)
        {
            // Blittable-only: trivial forwarding call (JIT will inline).
            sb.Append("        ");
            if (hasReturn) sb.Append("return ");
            sb.Append('_').Append(m.MethodName).Append('(');
            AppendCallArgs(sb, m.Parameters);
            sb.AppendLine(");");
            sb.AppendLine("    }");
            return;
        }

        // Has Span/String params — need unsafe + fixed.
        var spanParams = new List<ParameterInfo>();
        var stringParams = new List<ParameterInfo>();
        foreach (var p in m.Parameters)
        {
            if (p.Strategy == ParameterStrategy.SpanByte)
                spanParams.Add(p);
            else if (p.Strategy == ParameterStrategy.String)
                stringParams.Add(p);
        }

        // Emit UTF-8 encoding for string params (before the unsafe block).
        foreach (var p in stringParams)
        {
            sb.Append("        var __bytes_").Append(p.Name)
              .Append(" = global::System.Text.Encoding.UTF8.GetBytes(")
              .Append(p.Name).AppendLine(");");
        }

        sb.AppendLine("        unsafe");
        sb.AppendLine("        {");

        // Emit nested fixed statements.
        int indent = 3;
        foreach (var p in spanParams)
        {
            Indent(sb, indent);
            sb.Append("fixed (byte* __ptr_").Append(p.Name)
              .Append(" = ").Append(p.Name).AppendLine(")");
            indent++;
        }
        foreach (var p in stringParams)
        {
            Indent(sb, indent);
            sb.Append("fixed (byte* __ptr_").Append(p.Name)
              .Append(" = __bytes_").Append(p.Name).AppendLine(")");
            indent++;
        }

        Indent(sb, indent);
        sb.AppendLine("{");
        Indent(sb, indent + 1);
        if (hasReturn) sb.Append("return ");
        sb.Append('_').Append(m.MethodName).Append('(');
        AppendCallArgs(sb, m.Parameters);
        sb.AppendLine(");");
        Indent(sb, indent);
        sb.AppendLine("}");

        sb.AppendLine("        }");
        sb.AppendLine("    }");
    }

    // ========================================================================
    // Helpers
    // ========================================================================

    private static void AppendCallArgs(StringBuilder sb, EquatableArray<ParameterInfo> parameters)
    {
        bool first = true;
        foreach (var p in parameters)
        {
            if (!first) sb.Append(", ");
            first = false;

            switch (p.Strategy)
            {
                case ParameterStrategy.SpanByte:
                    sb.Append("__ptr_").Append(p.Name)
                      .Append(", ").Append(p.Name).Append(".Length");
                    break;
                case ParameterStrategy.String:
                    sb.Append("__ptr_").Append(p.Name)
                      .Append(", __bytes_").Append(p.Name).Append(".Length");
                    break;
                default:
                    sb.Append(p.Name);
                    break;
            }
        }
    }

    private static void AppendOriginalParams(StringBuilder sb, EquatableArray<ParameterInfo> parameters)
    {
        bool first = true;
        foreach (var p in parameters)
        {
            if (!first) sb.Append(", ");
            first = false;
            sb.Append(MinimalTypeName(p.TypeDisplayName)).Append(' ').Append(p.Name);
        }
    }

    /// <summary>
    /// Strips the "global::" prefix from fully-qualified display names for readability.
    /// </summary>
    private static string MinimalTypeName(string fullyQualified)
    {
        if (fullyQualified.StartsWith("global::System."))
        {
            var short_ = fullyQualified.Substring("global::System.".Length);
            return short_ switch
            {
                "Byte" => "byte",
                "SByte" => "sbyte",
                "Int16" => "short",
                "UInt16" => "ushort",
                "Int32" => "int",
                "UInt32" => "uint",
                "Int64" => "long",
                "UInt64" => "ulong",
                "Single" => "float",
                "Double" => "double",
                "IntPtr" => "nint",
                "UIntPtr" => "nuint",
                "String" => "string",
                "ReadOnlySpan<byte>" => "ReadOnlySpan<byte>",
                _ => fullyQualified
            };
        }
        return fullyQualified.StartsWith("global::") ? fullyQualified.Substring("global::".Length) : fullyQualified;
    }

    private static void Indent(StringBuilder sb, int level)
    {
        for (int i = 0; i < level; i++)
            sb.Append("    ");
    }
}
