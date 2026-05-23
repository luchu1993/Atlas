using System.IO;
using System.Linq;
using System.Text;
using Atlas.Generators.Def;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;
using Xunit;

namespace Atlas.Generators.Tests;

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
        var manifestText = SourceText.From(TestManifest.Derive(defXml), Encoding.UTF8);
        var additionalTexts = new AdditionalText[]
        {
            new InMemoryAdditionalText("Avatar.def", defText),
            new InMemoryAdditionalText("entity_ids.xml", manifestText),
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

        // Each declared struct gets its own Register_<Name> method and the
        // bootstrap drives them via RegisterAllStructs.
        Assert.Contains("Register_ItemStack", code);
        Assert.Contains("Register_SkillEntry", code);
        Assert.Contains("RegisterAllStructs", code);

        // Missing this leaves the native struct table empty.
        Assert.Contains("EntityRegistryBridge.RegisterStruct", code);

        // DefBootstrap is the single ModuleInitializer; per-emitter ones are gone.
        var bootstrap = FindGenerated(result, "DefBootstrap");
        Assert.NotNull(bootstrap);
        Assert.Contains("ModuleInitializer", bootstrap);
        Assert.Contains("DefStructRegistry.RegisterAllStructs()", bootstrap);
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
    public void ClientBootstrap_EmitsSessionRegistrationEntryPoint()
    {
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount) {}
}
";
        var xml = @"<entity name=""Avatar"">
  <client_methods>
    <method name=""ShowDamage"">
      <arg name=""amount"" type=""int32"" />
    </method>
  </client_methods>
</entity>";
        var result = Run(source, xml, "ATLAS_CLIENT");

        var bootstrap = FindGenerated(result, "DefBootstrap")!;
        var factory = FindGenerated(result, "EntityFactory")!;
        var dispatcher = FindGenerated(result, "DefRpcDispatcher")!;

        Assert.Contains("public static void RegisterInto(Atlas.Client.ClientSession session)",
                        bootstrap);
        Assert.Contains("DefEntityFactoryRegistrations.RegisterInto(session.EntityFactory)",
                        bootstrap);
        Assert.Contains("Atlas.Rpc.DefRpcDispatcher.RegisterInto(session)", bootstrap);
        Assert.Contains("internal static void RegisterInto(Atlas.Client.ClientEntityFactoryRegistry registry)",
                        factory);
        Assert.Contains("registry.Register", factory);
        Assert.Contains("internal static void RegisterInto(Atlas.Client.ClientSession session)",
                        dispatcher);
        Assert.Contains("session.ClientRpcDispatcher = DispatchClientRpc", dispatcher);
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

        // Alpha=1, Beta=2, Zeta=3 (ordinal). Match `void Register_<N>` not the call site.
        var alphaRegion = code.Substring(code.IndexOf("void Register_Alpha", System.StringComparison.Ordinal));
        Assert.Contains("WriteUInt16((ushort)1)", alphaRegion.Substring(0, 400));
        var betaRegion = code.Substring(code.IndexOf("void Register_Beta", System.StringComparison.Ordinal));
        Assert.Contains("WriteUInt16((ushort)2)", betaRegion.Substring(0, 400));
        var zetaRegion = code.Substring(code.IndexOf("void Register_Zeta", System.StringComparison.Ordinal));
        Assert.Contains("WriteUInt16((ushort)3)", zetaRegion.Substring(0, 400));
    }

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

        // Body has no redundant kind byte; elem ref leads with its own kind.
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

        // 3 emissions = property count + rpc count + slot count; no max_size.
        var count = System.Text.RegularExpressions.Regex.Matches(
            code, @"WritePackedUInt32\(").Count;
        Assert.Equal(3, count);
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

        // Two `WriteByte(16)` (prop header + nested elem) + one `WriteByte(5)`.
        var listBytes = System.Text.RegularExpressions.Regex.Matches(
            code, @"WriteByte\(16\)").Count;
        Assert.True(listBytes >= 2, $"expected ≥ 2 WriteByte(16) calls, got {listBytes}");
        Assert.Contains("WriteByte(5)", code);
    }

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

        // Cycle → parser rejects → generator emits nothing for the def, and
        // no struct registry since there's nothing to register.
        Assert.Null(FindGenerated(result, "DefStructRegistry"));
        Assert.Null(FindGenerated(result, "DefEntityTypeRegistry"));
    }
}
