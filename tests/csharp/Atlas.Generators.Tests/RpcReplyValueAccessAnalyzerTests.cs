using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Atlas.Generators.Def.Analyzers;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Xunit;

namespace Atlas.Generators.Tests;

public sealed class RpcReplyValueAccessAnalyzerTests
{
    // Stand-in for Atlas.Coro.Rpc.RpcReply<T> so the analyzer can resolve
    // the type without dragging the whole runtime assembly in.
    private const string RpcReplyStub = @"
namespace Atlas.Coro.Rpc
{
    public readonly struct RpcReply<T>
    {
        public T Value => default!;
        public int Error => 0;
        public string? ErrorMessage => null;
        public bool IsOk => true;
        public bool IsBusinessError => false;
        public bool IsFrameworkError => false;
        public bool TryGetValue(out T value) { value = default!; return true; }
        public static RpcReply<T> Ok(T v) => default;
        public static RpcReply<T> Fail(int code, string? m) => default;
    }
}
";

    private static async Task<List<Diagnostic>> GetDiagnosticsAsync(string userCode)
    {
        var compilation = CSharpCompilation.Create("Test",
            new[]
            {
                CSharpSyntaxTree.ParseText(RpcReplyStub),
                CSharpSyntaxTree.ParseText(userCode),
            },
            new[]
            {
                MetadataReference.CreateFromFile(typeof(object).Assembly.Location),
            },
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));

        var runtimeDir = Path.GetDirectoryName(typeof(object).Assembly.Location)!;
        var systemRuntime = Path.Combine(runtimeDir, "System.Runtime.dll");
        if (File.Exists(systemRuntime))
            compilation = compilation.AddReferences(MetadataReference.CreateFromFile(systemRuntime));

        var withAnalyzer = compilation.WithAnalyzers(
            System.Collections.Immutable.ImmutableArray.Create<DiagnosticAnalyzer>(
                new RpcReplyValueAccessAnalyzer()));
        var diags = await withAnalyzer.GetAnalyzerDiagnosticsAsync();
        return diags.Where(d => d.Id == "ATLAS_RPC001").ToList();
    }

    [Fact]
    public async Task Warns_WhenValueAccessedWithoutGuard()
    {
        const string code = @"
using Atlas.Coro.Rpc;
class Test {
    void M(RpcReply<int> r) {
        var v = r.Value;
    }
}";
        var diags = await GetDiagnosticsAsync(code);
        Assert.Single(diags);
    }

    [Fact]
    public async Task NoWarning_WhenIsOkChecked()
    {
        const string code = @"
using Atlas.Coro.Rpc;
class Test {
    void M(RpcReply<int> r) {
        if (r.IsOk) { var v = r.Value; }
    }
}";
        var diags = await GetDiagnosticsAsync(code);
        Assert.Empty(diags);
    }

    [Fact]
    public async Task NoWarning_WhenTryGetValueUsed()
    {
        const string code = @"
using Atlas.Coro.Rpc;
class Test {
    void M(RpcReply<int> r) {
        if (!r.TryGetValue(out _)) return;
        var v = r.Value;
    }
}";
        var diags = await GetDiagnosticsAsync(code);
        Assert.Empty(diags);
    }

    [Fact]
    public async Task NoWarning_WhenIsFrameworkErrorChecked()
    {
        const string code = @"
using Atlas.Coro.Rpc;
class Test {
    void M(RpcReply<int> r) {
        if (r.IsFrameworkError) return;
        var v = r.Value;
    }
}";
        var diags = await GetDiagnosticsAsync(code);
        Assert.Empty(diags);
    }

    [Fact]
    public async Task DoesNotMatch_NonRpcReplyValueAccess()
    {
        const string code = @"
class Holder { public int Value => 1; }
class Test {
    void M(Holder h) { var v = h.Value; }
}";
        var diags = await GetDiagnosticsAsync(code);
        Assert.Empty(diags);
    }

    [Fact]
    public async Task SeparateReceivers_GuardOnOneDoesNotProtectOther()
    {
        const string code = @"
using Atlas.Coro.Rpc;
class Test {
    void M(RpcReply<int> a, RpcReply<int> b) {
        if (a.IsOk) { var x = b.Value; }   // guard on a, accessing b.Value
    }
}";
        var diags = await GetDiagnosticsAsync(code);
        Assert.Single(diags);
    }
}
