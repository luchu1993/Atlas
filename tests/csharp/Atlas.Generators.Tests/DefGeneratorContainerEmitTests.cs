using System.IO;
using System.Linq;
using System.Text;
using Atlas.Generators.Def;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;
using Xunit;

namespace Atlas.Generators.Tests;

// End-to-end generator tests for the container-property emitter wiring:
// DefStructRegistry.RegisterAllStructs + DefEntityTypeRegistry container
// tails. Exercises the full pipeline (DefParser → DefLinker → emitters)
// driven from XML, so a regression anywhere in the chain surfaces as a
// missing / wrong generated code fragment.
public class DefGeneratorContainerEmitTests
{
    // Stubs let the generated code compile against a fake Atlas.Core /
    // Atlas.Client / Atlas.Serialization surface.
    private const string StubTypes = @"
using System;
namespace Atlas.Entity
{
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class EntityAttribute : Attribute
    {
        public string TypeName { get; }
        public EntityAttribute(string typeName) => TypeName = typeName;
    }
    public abstract class ServerEntity {
        public abstract void Serialize(ref Atlas.Serialization.SpanWriter w);
        public abstract void Deserialize(ref Atlas.Serialization.SpanReader r);
        public virtual string TypeName => """";
    }
}
namespace Atlas.Serialization
{
    public ref struct SpanWriter
    {
        public SpanWriter(int capacity) { }
        public void WriteInt8(sbyte v) {}
        public void WriteUInt8(byte v) {}
        public void WriteInt16(short v) {}
        public void WriteInt32(int v) { }
        public void WriteUInt32(uint v) { }
        public void WriteUInt16(ushort v) { }
        public void WriteInt64(long v) {}
        public void WriteUInt64(ulong v) {}
        public void WriteFloat(float v) {}
        public void WriteDouble(double v) {}
        public void WriteString(string value) { }
        public void WriteBytes(byte[] v) {}
        public void WriteBool(bool value) { }
        public void WriteByte(byte value) { }
        public void WritePackedUInt32(uint v) { }
        public readonly System.ReadOnlySpan<byte> WrittenSpan => default;
        public void Dispose() { }
    }
    public ref struct SpanReader
    {
        public SpanReader(System.ReadOnlySpan<byte> data) { }
        public int ReadInt32() => 0;
        public bool ReadBool() => false;
    }
}
namespace Atlas.Core
{
    public static class EntityRegistryBridge
    {
        public static void RegisterEntityType(System.ReadOnlySpan<byte> data) { }
        public static void RegisterStruct(System.ReadOnlySpan<byte> data) { }
    }
    public static class RpcBridge
    {
        public static void SendClientRpc(uint entityId, uint rpcId, System.ReadOnlySpan<byte> payload) { }
        public static void SendCellRpc(uint entityId, uint rpcId, System.ReadOnlySpan<byte> payload) { }
        public static void SendBaseRpc(uint entityId, uint rpcId, System.ReadOnlySpan<byte> payload) { }
    }
}
namespace Atlas.Client
{
    public abstract class ClientEntity { public virtual string TypeName => """"; }
    public static class ClientEntityRegistryBridge {
        public static void RegisterEntityType(System.ReadOnlySpan<byte> data) { }
        public static void RegisterStruct(System.ReadOnlySpan<byte> data) { }
    }
    public static class ClientRpcBridge {
        public static void SendBaseRpc(uint entityId, uint rpcId, System.ReadOnlySpan<byte> payload) { }
        public static void SendCellRpc(uint entityId, uint rpcId, System.ReadOnlySpan<byte> payload) { }
    }
}
";

    private sealed class InMemoryAdditionalText : AdditionalText
    {
        public InMemoryAdditionalText(string path, SourceText text) { Path = path; _text = text; }
        public override string Path { get; }
        private readonly SourceText _text;
        public override SourceText GetText(System.Threading.CancellationToken ct = default) => _text;
    }

    private static GeneratorDriverRunResult Run(string userSource, string defXml,
                                                string preprocessorSymbol = "ATLAS_BASE")
    {
        var parseOptions = CSharpParseOptions.Default
            .WithLanguageVersion(LanguageVersion.Latest);
        if (!string.IsNullOrEmpty(preprocessorSymbol))
            parseOptions = parseOptions.WithPreprocessorSymbols(preprocessorSymbol);

        var compilation = CSharpCompilation.Create("TestAssembly",
            new[]
            {
                CSharpSyntaxTree.ParseText(StubTypes, parseOptions),
                CSharpSyntaxTree.ParseText(userSource, parseOptions),
            },
            new[]
            {
                MetadataReference.CreateFromFile(typeof(object).Assembly.Location),
                MetadataReference.CreateFromFile(typeof(System.Attribute).Assembly.Location),
            },
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));

        var runtimeDir = Path.GetDirectoryName(typeof(object).Assembly.Location)!;
        var systemRuntime = Path.Combine(runtimeDir, "System.Runtime.dll");
        if (File.Exists(systemRuntime))
            compilation = compilation.AddReferences(MetadataReference.CreateFromFile(systemRuntime));

        var defText = SourceText.From(defXml, Encoding.UTF8);
        var additionalTexts = new AdditionalText[]
        {
            new InMemoryAdditionalText("Avatar.def", defText)
        };

        var driver = CSharpGeneratorDriver.Create(
            generators: new[] { new DefGenerator().AsSourceGenerator() },
            additionalTexts: additionalTexts,
            parseOptions: parseOptions);

        return driver.RunGenerators(compilation).GetRunResult();
    }

    private static string? FindGenerated(GeneratorDriverRunResult result, string fragment)
    {
        var tree = result.GeneratedTrees.FirstOrDefault(t => t.FilePath.Contains(fragment));
        return tree?.GetText().ToString();
    }

    private const string MinimalUserSource = @"
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
}
";

    // =========================================================================
    // Struct registry — emitted when <types> section is present
    // =========================================================================

    [Fact]
    public void StructRegistry_EmittedForEachDeclaredStruct()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack"">
      <field name=""id"" type=""int32"" />
      <field name=""count"" type=""uint16"" />
    </struct>
    <struct name=""SkillEntry"">
      <field name=""id"" type=""uint16"" />
      <field name=""level"" type=""uint8"" />
    </struct>
  </types>
  <properties>
    <property name=""hp"" type=""int32"" scope=""all_clients"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "DefStructRegistry");
        Assert.NotNull(code);

        // Each declared struct gets its own Register_<Name> method and a
        // corresponding call from the module initializer.
        Assert.Contains("Register_ItemStack", code);
        Assert.Contains("Register_SkillEntry", code);
        Assert.Contains("ModuleInitializer", code);
        Assert.Contains("RegisterAllStructs", code);

        // Both register methods must feed the bridge; missing this means
        // the struct table on the native side stays empty and any
        // RegisterEntityType that references a struct_id will reject.
        Assert.Contains("EntityRegistryBridge.RegisterStruct", code);
    }

    [Fact]
    public void StructRegistry_NotEmittedWhenNoStructsDeclared()
    {
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp"" type=""int32"" scope=""all_clients"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "DefStructRegistry");
        // Zero struct declarations → no registry file at all. Keeps the
        // generated surface minimal for scalar-only defs.
        Assert.Null(code);
    }

    [Fact]
    public void StructRegistry_AssignsStableIdsAlphabetically()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""Zeta""><field name=""v"" type=""int32"" /></struct>
    <struct name=""Alpha""><field name=""v"" type=""int32"" /></struct>
    <struct name=""Beta""><field name=""v"" type=""int32"" /></struct>
  </types>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "DefStructRegistry")!;

        // Alphabetical: Alpha=1, Beta=2, Zeta=3. Deterministic ordering is
        // what lets the emitter and DefLinker agree without shared state.
        // Look for the method definition (`void Register_<Name>`) rather
        // than the call site, which also contains `Register_<Name>(`.
        var alphaRegion = code.Substring(code.IndexOf("void Register_Alpha", System.StringComparison.Ordinal));
        Assert.Contains("WriteUInt16((ushort)1)", alphaRegion.Substring(0, 400));
        var betaRegion = code.Substring(code.IndexOf("void Register_Beta", System.StringComparison.Ordinal));
        Assert.Contains("WriteUInt16((ushort)2)", betaRegion.Substring(0, 400));
        var zetaRegion = code.Substring(code.IndexOf("void Register_Zeta", System.StringComparison.Ordinal));
        Assert.Contains("WriteUInt16((ushort)3)", zetaRegion.Substring(0, 400));
    }

    // =========================================================================
    // Entity type registry — container property tail
    // =========================================================================

    [Fact]
    public void EntityTypeRegistry_EmitsListPropertyTail()
    {
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""titles"" type=""list[int32]"" scope=""own_client"" max_size=""128"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "DefEntityTypeRegistry")!;

        // Top-level kind byte for the property: kList = 16. No leading
        // redundant kind byte on the body; the elem DataTypeRef starts
        // with its own kind byte (kInt32 = 5).
        Assert.Contains("WriteByte(16)", code);   // prop.data_type = List
        Assert.Contains("WriteByte(5)", code);    // list.elem.kind = Int32
        Assert.Contains("WritePackedUInt32(128)", code);  // max_size
    }

    [Fact]
    public void EntityTypeRegistry_EmitsDictPropertyTail()
    {
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""counters"" type=""dict[string,int32]"" scope=""own_client"" max_size=""64"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "DefEntityTypeRegistry")!;

        // Top-level kind = kDict (17). Body contains two nested DataTypeRefs:
        // key (kString = 11) then value (kInt32 = 5).
        Assert.Contains("WriteByte(17)", code);
        Assert.Contains("WriteByte(11)", code);
        Assert.Contains("WriteByte(5)", code);
        Assert.Contains("WritePackedUInt32(64)", code);
    }

    [Fact]
    public void EntityTypeRegistry_EmitsStructPropertyTail_WithResolvedStructId()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
  </types>
  <properties>
    <property name=""weapon"" type=""ItemStack"" scope=""all_clients"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "DefEntityTypeRegistry")!;

        // kStruct = 18. Only one struct is declared, so DefLinker assigns
        // id=1. The property tail is just the struct_id (2 bytes).
        Assert.Contains("WriteByte(18)", code);
        Assert.Contains("WriteUInt16((ushort)1)", code);
        // Default max_size still written for container properties.
        Assert.Contains("WritePackedUInt32(4096)", code);
    }

    [Fact]
    public void EntityTypeRegistry_ScalarPropertyHasNoContainerTail()
    {
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp"" type=""int32"" scope=""all_clients"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "DefEntityTypeRegistry")!;

        // The only WritePackedUInt32 in the generated method should be the
        // property count + rpc count (each emitted once); there should NOT
        // be a max_size emission. We check by counting: exactly 2 matches.
        var count = System.Text.RegularExpressions.Regex.Matches(
            code, @"WritePackedUInt32\(").Count;
        Assert.Equal(2, count);
    }

    [Fact]
    public void EntityTypeRegistry_NestedListListEmitsTwoKindBytes()
    {
        var xml = @"<entity name=""Grid"">
  <properties>
    <property name=""rows"" type=""list[list[int32]]"" scope=""own_client"" />
  </properties>
</entity>";
        var userSource = MinimalUserSource.Replace("Avatar", "Grid");
        var result = Run(userSource, xml);
        var code = FindGenerated(result, "DefEntityTypeRegistry")!;

        // Top-level prop.data_type = kList (emitted once in prop header).
        // Body: outer list omits its own kind byte; nested elem IS a full
        // DataTypeRef so it emits `WriteByte(16)` (kList) then elem's
        // kind `WriteByte(5)` (kInt32). Overall:
        //   prop top-level WriteByte(16)   → 1
        //   nested elem WriteByte(16)      → 2 (same byte value; count duplicates)
        //   nested elem's elem WriteByte(5) → at least one
        var listBytes = System.Text.RegularExpressions.Regex.Matches(
            code, @"WriteByte\(16\)").Count;
        Assert.True(listBytes >= 2, $"expected ≥ 2 WriteByte(16) calls, got {listBytes}");
        Assert.Contains("WriteByte(5)", code);
    }

    // =========================================================================
    // End-to-end pipeline — cycle / link diagnostics
    // =========================================================================

    [Fact]
    public void Generator_CyclicStructsProduceNoOutput()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""A""><field name=""b"" type=""B"" /></struct>
    <struct name=""B""><field name=""a"" type=""A"" /></struct>
  </types>
</entity>";
        var result = Run(MinimalUserSource, xml);

        // Parser rejects the cycle and returns null → generator drops the
        // def before emitting anything entity-specific. The struct registry
        // must not be emitted either (nothing to register). The single
        // generator failure path: empty entity list, no RegisterAllStructs.
        Assert.Null(FindGenerated(result, "DefStructRegistry"));
        Assert.Null(FindGenerated(result, "DefEntityTypeRegistry"));
    }
}
