using System.IO;
using System.Linq;
using Atlas.Generators.Def;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;
using Xunit;

namespace Atlas.Generators.Tests;

public class DefGeneratorTests
{
    // Stub types that the generator needs to compile against
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
}
namespace Atlas.Serialization
{
    public ref struct SpanWriter
    {
        public SpanWriter(int capacity) { }
        public void WriteInt32(int value) { }
        public void WriteUInt32(uint value) { }
        public void WriteFloat(float value) { }
        public void WriteString(string value) { }
        public void WriteBool(bool value) { }
        public readonly System.ReadOnlySpan<byte> WrittenSpan => default;
        public void Dispose() { }
    }
    public ref struct SpanReader
    {
        public SpanReader(System.ReadOnlySpan<byte> data) { }
        public int ReadInt32() => 0;
        public uint ReadUInt32() => 0;
        public float ReadFloat() => 0f;
        public string ReadString() => """";
        public bool ReadBool() => false;
    }
}
namespace Atlas.Core
{
    public static class RpcBridge
    {
        public delegate void RpcDispatchDelegate(Atlas.Entity.ServerEntity entity, int rpcId, ref Atlas.Serialization.SpanReader reader);
        public static readonly RpcDispatchDelegate[] Dispatchers = new RpcDispatchDelegate[4];
    }
}
namespace Atlas.Entity
{
    public abstract class ServerEntity
    {
        public uint EntityId { get; internal set; }
        public abstract string TypeName { get; }
        protected internal void SendClientRpc(int rpcId, System.ReadOnlySpan<byte> payload) { }
        protected internal void SendCellRpc(int rpcId, System.ReadOnlySpan<byte> payload) { }
        protected internal void SendBaseRpc(int rpcId, System.ReadOnlySpan<byte> payload) { }
    }
}
namespace Atlas.Client
{
    public abstract class ClientEntity
    {
        public uint EntityId { get; internal set; }
        public abstract string TypeName { get; }
        protected internal void SendCellRpc(int rpcId, System.ReadOnlySpan<byte> payload) { }
        protected internal void SendBaseRpc(int rpcId, System.ReadOnlySpan<byte> payload) { }
    }
    public static class ClientCallbacks
    {
        public delegate void RpcDispatchDelegate(ClientEntity entity, int rpcId, ref Atlas.Serialization.SpanReader reader);
        public static RpcDispatchDelegate ClientRpcDispatcher;
    }
}
";

    private const string AvatarDef = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp""       type=""int32""   scope=""all_clients""     persistent=""true"" />
    <property name=""gold""     type=""int32""   scope=""base""            persistent=""true"" />
    <property name=""aiState""  type=""int32""   scope=""cell_public"" />
    <property name=""modelId""  type=""int32""   scope=""other_clients"" />
    <property name=""mana""     type=""int32""   scope=""cell_public_and_own"" />
    <property name=""secret""   type=""string""  scope=""base_and_client""  persistent=""true"" />
    <property name=""level""    type=""int32""   scope=""own_client""       persistent=""true"" />
    <property name=""pos""      type=""vector3"" scope=""cell_private"" />
  </properties>
  <client_methods>
    <method name=""ShowDamage"">
      <arg name=""amount""     type=""int32"" />
      <arg name=""attackerId"" type=""uint32"" />
    </method>
  </client_methods>
  <cell_methods>
    <method name=""CastSkill"" exposed=""own_client"">
      <arg name=""skillId""  type=""int32"" />
      <arg name=""targetId"" type=""uint32"" />
    </method>
    <method name=""OnEnterRegion"">
      <arg name=""regionId"" type=""int32"" />
    </method>
  </cell_methods>
  <base_methods>
    <method name=""UseItem"" exposed=""own_client"">
      <arg name=""itemId"" type=""int32"" />
    </method>
    <method name=""OnPlayerDead"" />
  </base_methods>
</entity>";

    private (GeneratorDriverRunResult Result, Compilation Compilation) RunDefGenerator(
        string userSource, string defXml, string preprocessorSymbol = "")
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

        var generator = new DefGenerator();
        var defText = SourceText.From(defXml, System.Text.Encoding.UTF8);
        var additionalTexts = new AdditionalText[] { new InMemoryAdditionalText("Avatar.def", defText) };

        var driver = CSharpGeneratorDriver.Create(
            generators: new[] { generator.AsSourceGenerator() },
            additionalTexts: additionalTexts,
            parseOptions: parseOptions);

        var runResult = driver.RunGenerators(compilation).GetRunResult();
        return (runResult, compilation);
    }

    // =========================================================================
    // DefParser tests
    // =========================================================================

    [Fact]
    public void DefParser_ParsesEntityName()
    {
        var text = SourceText.From(AvatarDef, System.Text.Encoding.UTF8);
        var model = DefParser.Parse(text, "Avatar.def", null);

        Assert.NotNull(model);
        Assert.Equal("Avatar", model!.Name);
    }

    [Fact]
    public void DefParser_ParsesAllPropertyScopes()
    {
        var text = SourceText.From(AvatarDef, System.Text.Encoding.UTF8);
        var model = DefParser.Parse(text, "Avatar.def", null)!;

        Assert.Equal(8, model.Properties.Count);
        Assert.Equal(PropertyScope.AllClients, model.Properties[0].Scope);       // hp
        Assert.Equal(PropertyScope.Base, model.Properties[1].Scope);              // gold
        Assert.Equal(PropertyScope.CellPublic, model.Properties[2].Scope);        // aiState
        Assert.Equal(PropertyScope.OtherClients, model.Properties[3].Scope);      // modelId
        Assert.Equal(PropertyScope.CellPublicAndOwn, model.Properties[4].Scope);  // mana
        Assert.Equal(PropertyScope.BaseAndClient, model.Properties[5].Scope);     // secret
        Assert.Equal(PropertyScope.OwnClient, model.Properties[6].Scope);         // level
        Assert.Equal(PropertyScope.CellPrivate, model.Properties[7].Scope);       // pos
    }

    [Fact]
    public void DefParser_ParsesMethodsAndArgs()
    {
        var text = SourceText.From(AvatarDef, System.Text.Encoding.UTF8);
        var model = DefParser.Parse(text, "Avatar.def", null)!;

        Assert.Single(model.ClientMethods);
        Assert.Equal("ShowDamage", model.ClientMethods[0].Name);
        Assert.Equal(2, model.ClientMethods[0].Args.Count);

        Assert.Equal(2, model.CellMethods.Count);
        Assert.Equal("CastSkill", model.CellMethods[0].Name);
        Assert.Equal(ExposedScope.OwnClient, model.CellMethods[0].Exposed);
        Assert.Equal("OnEnterRegion", model.CellMethods[1].Name);
        Assert.Equal(ExposedScope.None, model.CellMethods[1].Exposed);

        Assert.Equal(2, model.BaseMethods.Count);
        Assert.Equal("UseItem", model.BaseMethods[0].Name);
        Assert.Equal(ExposedScope.OwnClient, model.BaseMethods[0].Exposed);
        Assert.Equal("OnPlayerDead", model.BaseMethods[1].Name);
        Assert.Equal(ExposedScope.None, model.BaseMethods[1].Exposed);
    }

    [Fact]
    public void DefParser_InfersHasCellAndHasClient()
    {
        var text = SourceText.From(AvatarDef, System.Text.Encoding.UTF8);
        var model = DefParser.Parse(text, "Avatar.def", null)!;

        Assert.True(model.HasCell);
        Assert.True(model.HasClient);
    }

    [Fact]
    public void DefParser_BaseOnlyEntity_HasNoCell()
    {
        var xml = @"<entity name=""Account"">
  <base_methods>
    <method name=""Login"" exposed=""own_client"" />
  </base_methods>
</entity>";
        var model = DefParser.Parse(SourceText.From(xml, System.Text.Encoding.UTF8), "Account.def", null)!;

        Assert.False(model.HasCell);
        Assert.False(model.HasClient);
    }

    // =========================================================================
    // DefParser validation tests
    // =========================================================================

    [Fact]
    public void DefParser_ClientMethodWithExposed_ReportsDEF002()
    {
        var xml = @"<entity name=""Bad"">
  <client_methods>
    <method name=""ShowEffect"" exposed=""own_client"">
      <arg name=""id"" type=""int32"" />
    </method>
  </client_methods>
</entity>";
        Diagnostic? reported = null;
        var model = DefParser.Parse(
            SourceText.From(xml, System.Text.Encoding.UTF8), "Bad.def",
            d => reported = d);

        Assert.NotNull(reported);
        Assert.Equal("ATLAS_DEF002", reported!.Id);
        // Exposed should be reset to None
        Assert.Equal(ExposedScope.None, model!.ClientMethods[0].Exposed);
    }

    [Fact]
    public void DefParser_BaseMethodWithAllClients_ReportsDEF003()
    {
        var xml = @"<entity name=""Bad"">
  <base_methods>
    <method name=""DoSomething"" exposed=""all_clients"">
      <arg name=""x"" type=""int32"" />
    </method>
  </base_methods>
</entity>";
        Diagnostic? reported = null;
        var model = DefParser.Parse(
            SourceText.From(xml, System.Text.Encoding.UTF8), "Bad.def",
            d => reported = d);

        Assert.NotNull(reported);
        Assert.Equal("ATLAS_DEF003", reported!.Id);
        // Exposed should be downgraded to OwnClient
        Assert.Equal(ExposedScope.OwnClient, model!.BaseMethods[0].Exposed);
    }

    // =========================================================================
    // Generator output tests — Base context
    // =========================================================================

    [Fact]
    public void BaseContext_GeneratesRpcIds()
    {
        var source = @"
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
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_BASE");

        Assert.Empty(result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error));

        var files = result.GeneratedTrees.Select(t => Path.GetFileName(t.FilePath)).ToList();
        Assert.Contains("RpcIds.g.cs", files);
    }

    [Fact]
    public void BaseContext_BaseMethodsArePartial()
    {
        var source = @"
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
    public partial void UseItem(int itemId) { }
    public partial void OnPlayerDead() { }
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_BASE");

        var stubs = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.RpcStubs"));
        Assert.NotNull(stubs);

        var code = stubs!.GetText().ToString();
        // base_methods should be partial (Receive role)
        Assert.Contains("public partial void UseItem(int itemId);", code);
        Assert.Contains("public partial void OnPlayerDead();", code);
        // client_methods should be send stubs (not partial)
        Assert.Contains("public void ShowDamage(", code);
        Assert.DoesNotContain("public partial void ShowDamage(", code);
    }

    [Fact]
    public void BaseContext_GeneratesClientMailbox()
    {
        var source = @"
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
    public partial void UseItem(int itemId) { }
    public partial void OnPlayerDead() { }
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_BASE");

        var mailboxes = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.Mailboxes"));
        Assert.NotNull(mailboxes);

        var code = mailboxes!.GetText().ToString();
        Assert.Contains("AvatarClientMailbox", code);
        Assert.Contains("public AvatarClientMailbox Client =>", code);
    }

    // =========================================================================
    // Generator output tests — Client context
    // =========================================================================

    [Fact]
    public void ClientContext_ClientMethodsArePartial()
    {
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) { }
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");

        var stubs = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.RpcStubs"));
        Assert.NotNull(stubs);

        var code = stubs!.GetText().ToString();
        // client_methods should be partial (Receive role)
        Assert.Contains("public partial void ShowDamage(int amount, uint attackerId);", code);
        // Exposed base_methods should be send stubs
        Assert.Contains("public void UseItem(int itemId)", code);
        Assert.Contains("SendBaseRpc(", code);
        // Non-exposed base_methods should be forbidden
        Assert.Contains("public void OnPlayerDead()", code);
        Assert.Contains("throw new InvalidOperationException", code);
    }

    [Fact]
    public void ClientContext_ExposedCellMethodIsSendStub()
    {
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) { }
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");

        var code = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.RpcStubs"))
            .GetText().ToString();

        // CastSkill (exposed=own_client) → send stub
        Assert.Contains("public void CastSkill(int skillId, uint targetId)", code);
        Assert.Contains("SendCellRpc(", code);
        // OnEnterRegion (not exposed) → forbidden
        Assert.Contains("public void OnEnterRegion(int regionId)", code);
        Assert.Contains("throw new InvalidOperationException", code);
    }

    [Fact]
    public void ClientContext_GeneratesBaseMailbox()
    {
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) { }
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");

        var mailboxes = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.Mailboxes"));
        Assert.NotNull(mailboxes);

        var code = mailboxes!.GetText().ToString();
        // Client should have Base and Cell mailboxes
        Assert.Contains("AvatarBaseMailbox", code);
        Assert.Contains("AvatarCellMailbox", code);
        // But NOT a Client mailbox (clients don't send client methods)
        Assert.DoesNotContain("AvatarClientMailbox", code);
    }

    [Fact]
    public void ClientContext_BaseMailboxOnlyContainsExposedMethods()
    {
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) { }
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");

        var code = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.Mailboxes"))
            .GetText().ToString();

        // UseItem is exposed → should be in mailbox
        Assert.Contains("public void UseItem(int itemId)", code);
        // OnPlayerDead is NOT exposed → should NOT be in mailbox
        // Count occurrences of OnPlayerDead in the mailbox section
        Assert.DoesNotContain("OnPlayerDead", code);
    }

    // =========================================================================
    // RPC ID consistency test
    // =========================================================================

    [Fact]
    public void RpcIds_AreConsistentAcrossContexts()
    {
        var baseSource = @"
using Atlas.Entity;
using Atlas.Serialization;
[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
    public partial void UseItem(int itemId) {}
    public partial void OnPlayerDead() {}
}
";
        var clientSource = @"
using Atlas.Client;
[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var (baseResult, _) = RunDefGenerator(baseSource, AvatarDef, "ATLAS_BASE");
        var (clientResult, _) = RunDefGenerator(clientSource, AvatarDef, "ATLAS_CLIENT");

        var baseIds = baseResult.GeneratedTrees
            .First(t => t.FilePath.Contains("RpcIds")).GetText().ToString();
        var clientIds = clientResult.GeneratedTrees
            .First(t => t.FilePath.Contains("RpcIds")).GetText().ToString();

        // RPC IDs should be identical regardless of context
        Assert.Equal(baseIds, clientIds);
    }

    // =========================================================================
    // DEF001 diagnostic test
    // =========================================================================

    // =========================================================================
    // Reliable attribute tests (补强四 §4.1–§4.5)
    // =========================================================================

    [Fact]
    public void DefParser_ReliableAttribute_DefaultsFalse()
    {
        // AvatarDef (top of this file) does not mark any property reliable;
        // without the attribute Reliable must default to false.
        var text = SourceText.From(AvatarDef, System.Text.Encoding.UTF8);
        var model = DefParser.Parse(text, "Avatar.def", null)!;

        Assert.All(model.Properties, p => Assert.False(p.Reliable));
    }

    [Fact]
    public void DefParser_ReliableAttribute_ParsesTrue()
    {
        var xml = @"<entity name=""Npc"">
  <properties>
    <property name=""hp""       type=""int32""   scope=""all_clients""  reliable=""true"" />
    <property name=""position"" type=""vector3"" scope=""all_clients"" />
  </properties>
</entity>";
        var model = DefParser.Parse(SourceText.From(xml, System.Text.Encoding.UTF8), "Npc.def", null)!;

        Assert.True(model.Properties[0].Reliable);   // hp
        Assert.False(model.Properties[1].Reliable);  // position
    }

    [Fact]
    public void DeltaSync_EmitsReliableAndUnreliableMethods()
    {
        var reliableXml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp""       type=""int32""   scope=""all_clients"" reliable=""true"" />
    <property name=""mana""     type=""int32""   scope=""all_clients"" />
  </properties>
</entity>";
        var source = @"
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
        var (result, _) = RunDefGenerator(source, reliableXml, "ATLAS_BASE");

        var deltaTree = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.DeltaSync"));
        Assert.NotNull(deltaTree);
        var code = deltaTree!.GetText().ToString();

        // Per-channel masks partition the dirty bits.
        Assert.Contains("ReliableDirtyMask", code);
        Assert.Contains("UnreliableDirtyMask", code);
        // Reliable mask references hp (marked reliable="true").
        Assert.Matches(@"ReliableDirtyMask\s*=\s*ReplicatedDirtyFlags\.Hp\s*;", code);
        // Unreliable mask references mana (unmarked).
        Assert.Matches(@"UnreliableDirtyMask\s*=\s*ReplicatedDirtyFlags\.Mana\s*;", code);

        // Both serialize entry points are emitted.
        Assert.Contains("SerializeReplicatedDeltaReliable(", code);
        Assert.Contains("SerializeReplicatedDeltaUnreliable(", code);

        // Has-dirty helpers allow the send loop to skip empty channels cheaply.
        Assert.Contains("HasReliableDirty", code);
        Assert.Contains("HasUnreliableDirty", code);
    }

    [Fact]
    public void DeltaSync_AllUnreliable_EmitsEmptyReliableMask()
    {
        // Property marked all_clients but no reliable="true": unreliable only path.
        var xml = @"<entity name=""Drone"">
  <properties>
    <property name=""position"" type=""vector3"" scope=""all_clients"" />
  </properties>
</entity>";
        var source = @"
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Drone"")]
public partial class Drone : ServerEntity
{
    public override string TypeName => ""Drone"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
}
";
        var (result, _) = RunDefGenerator(source, xml, "ATLAS_BASE");

        var deltaTree = result.GeneratedTrees.FirstOrDefault(t => t.FilePath.Contains("DeltaSync"));
        Assert.NotNull(deltaTree);
        var code = deltaTree!.GetText().ToString();

        // Reliable mask has no members → expressed as ReplicatedDirtyFlags.None.
        Assert.Matches(@"ReliableDirtyMask\s*=\s*ReplicatedDirtyFlags\.None\s*;", code);
        // No reliable-channel serialize method is emitted when nothing is reliable.
        Assert.DoesNotContain("SerializeReplicatedDeltaReliable(", code);
        // Unreliable path still exists.
        Assert.Contains("SerializeReplicatedDeltaUnreliable(", code);
    }

    // =========================================================================
    // Baseline snapshot support (补强一)
    // =========================================================================

    [Fact]
    public void DeltaSync_OwnerScopeMethod_IsOverrideOnServerEntity()
    {
        // 补强一 depends on SerializeForOwnerClient being virtually dispatchable
        // from NativeCallbacks.GetOwnerSnapshot, not a static per-class method.
        // Guard against someone accidentally removing the `override` modifier.
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp""    type=""int32""   scope=""all_clients"" />
    <property name=""level"" type=""int32""   scope=""own_client"" />
  </properties>
</entity>";
        var source = @"
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
        var (result, _) = RunDefGenerator(source, xml, "ATLAS_BASE");

        var deltaTree = result.GeneratedTrees.FirstOrDefault(t => t.FilePath.Contains("DeltaSync"));
        Assert.NotNull(deltaTree);
        var code = deltaTree!.GetText().ToString();

        Assert.Contains("public override void SerializeForOwnerClient(", code);
    }

    [Fact]
    public void TypeRegistryEmitter_EmitsReliableByte()
    {
        // The emitter must append an explicit reliable flag per property so the
        // C++ registry reader can deserialize it deterministically (protocol v2).
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp"" type=""int32"" scope=""all_clients"" reliable=""true"" />
    <property name=""position"" type=""vector3"" scope=""all_clients"" />
  </properties>
</entity>";
        var source = @"
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
        var (result, _) = RunDefGenerator(source, xml, "ATLAS_BASE");

        var registryTree = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("EntityTypeRegistry"));
        Assert.NotNull(registryTree);
        var code = registryTree!.GetText().ToString();

        // The emitter writes 'WriteBool(true)' for reliable hp and 'WriteBool(false)'
        // for the unmarked position. Assert both appear.
        Assert.Contains("WriteBool(true)", code);
        Assert.Contains("WriteBool(false)", code);
    }

    // =========================================================================
    // Phase B0 — paired scope-aware snapshot deserializers
    //
    // The AoI enter envelope carries bytes produced by the server's
    // SerializeForOtherClients; baseline carries SerializeForOwnerClient
    // output. These are raw-field scope subsets (no version/fieldCount/
    // bodyLength framing), so the client side needs paired Apply*Snapshot
    // methods that iterate the same scope filter in the same declaration
    // order — see PROPERTY_SYNC_DESIGN.md §9 / Phase B0.
    // =========================================================================

    // Pull the body of a method named `methodName` out of a generated file's
    // text. Naive but sufficient for these tests (one `{` / `}` nesting).
    private static string ExtractMethodBody(string code, string methodSignatureSubstring)
    {
        int sigIdx = code.IndexOf(methodSignatureSubstring, System.StringComparison.Ordinal);
        Assert.True(sigIdx >= 0,
            $"method '{methodSignatureSubstring}' not found in generated code:\n{code}");
        int braceOpen = code.IndexOf('{', sigIdx);
        Assert.True(braceOpen > 0, "opening brace not found");
        int depth = 1;
        int i = braceOpen + 1;
        while (i < code.Length && depth > 0)
        {
            if (code[i] == '{') depth++;
            else if (code[i] == '}') depth--;
            i++;
        }
        Assert.Equal(0, depth);
        return code.Substring(braceOpen + 1, i - braceOpen - 2);
    }

    [Fact]
    public void ClientContext_EmitsApplyOwnerSnapshot_WithOwnerVisibleFieldsInOrder()
    {
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");

        var delta = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.DeltaSync"));
        Assert.NotNull(delta);
        var code = delta!.GetText().ToString();

        // Owner-visible from AvatarDef (in declaration order): hp, mana, secret, level
        var body = ExtractMethodBody(code, "ApplyOwnerSnapshot(ref SpanReader reader)");
        int hp     = body.IndexOf("_hp = reader.ReadInt32()",  System.StringComparison.Ordinal);
        int mana   = body.IndexOf("_mana = reader.ReadInt32()",System.StringComparison.Ordinal);
        int secret = body.IndexOf("_secret = reader.ReadString()", System.StringComparison.Ordinal);
        int level  = body.IndexOf("_level = reader.ReadInt32()",System.StringComparison.Ordinal);

        Assert.True(hp >= 0 && mana > hp && secret > mana && level > secret,
            $"owner-visible fields not in declaration order; body was:\n{body}");

        // Fields outside the owner-scope must not appear.
        Assert.DoesNotContain("_gold",    body);  // scope=base
        Assert.DoesNotContain("_aiState", body);  // scope=cell_public
        Assert.DoesNotContain("_modelId", body);  // scope=other_clients (other-only)
        Assert.DoesNotContain("_pos",     body);  // scope=cell_private
    }

    [Fact]
    public void ClientContext_EmitsApplyOtherSnapshot_WithOtherVisibleFieldsInOrder()
    {
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");

        var code = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync"))
            .GetText().ToString();

        // Other-visible (AllClients + OtherClients): hp, modelId
        var body = ExtractMethodBody(code, "ApplyOtherSnapshot(ref SpanReader reader)");
        int hp      = body.IndexOf("_hp = reader.ReadInt32()",     System.StringComparison.Ordinal);
        int modelId = body.IndexOf("_modelId = reader.ReadInt32()",System.StringComparison.Ordinal);

        Assert.True(hp >= 0 && modelId > hp,
            $"other-visible fields not in declaration order; body was:\n{body}");

        // Owner-only fields must not appear.
        Assert.DoesNotContain("_mana",   body);  // cell_public_and_own → owner-only
        Assert.DoesNotContain("_secret", body);  // base_and_client     → owner-only
        Assert.DoesNotContain("_level",  body);  // own_client          → owner-only
    }

    [Fact]
    public void ClientContext_SnapshotApply_DoesNotFireChangeCallbacks()
    {
        // Initial / baseline snapshots are authoritative resets, not observed
        // changes — they must bypass the OnXxxChanged hook that the setter
        // path will fire in Phase B2. Guarantee: Apply*Snapshot writes to
        // the private backing field (`_<name>`) rather than the public
        // property setter, so no OnXxxChanged call can sneak in.
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");
        var code = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync"))
            .GetText().ToString();

        var ownerBody = ExtractMethodBody(code, "ApplyOwnerSnapshot(ref SpanReader reader)");
        var otherBody = ExtractMethodBody(code, "ApplyOtherSnapshot(ref SpanReader reader)");

        // No PascalCase property write (e.g. "Hp = ") and no OnXxxChanged call.
        Assert.DoesNotContain("Hp = reader",    ownerBody);
        Assert.DoesNotContain("Mana = reader",  ownerBody);
        Assert.DoesNotContain("OnHpChanged",    ownerBody);
        Assert.DoesNotContain("OnHpChanged",    otherBody);
        Assert.DoesNotContain("OnManaChanged",  ownerBody);
        Assert.DoesNotContain("OnModelIdChanged", otherBody);
    }

    [Fact]
    public void ClientContext_ScopeApplyOrder_MirrorsServerScopeSerialize()
    {
        // Load-bearing invariant: the server writes owner/other snapshots
        // with the same field filter + declaration-order iteration as the
        // client reads them. A regression where one side picks up a field
        // the other side doesn't will silently corrupt state on peer enter
        // or baseline. This test pins the order lock-step.
        var clientSource = @"
using Atlas.Client;
[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var baseSource = @"
using Atlas.Entity;
using Atlas.Serialization;
[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
    public partial void UseItem(int itemId) {}
    public partial void OnPlayerDead() {}
}
";
        var (clientResult, _) = RunDefGenerator(clientSource, AvatarDef, "ATLAS_CLIENT");
        var (baseResult,   _) = RunDefGenerator(baseSource,   AvatarDef, "ATLAS_BASE");

        var clientCode = clientResult.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync")).GetText().ToString();
        var baseCode = baseResult.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync")).GetText().ToString();

        // Extract field names from both sides in emission order.
        static System.Collections.Generic.List<string> FieldOrder(string methodBody)
        {
            // Matches '_fieldName' as the LHS of an assignment or as the
            // argument to writer.WriteX(_fieldName).
            var regex = new System.Text.RegularExpressions.Regex(@"_([a-zA-Z][a-zA-Z0-9]*)");
            var seen = new System.Collections.Generic.HashSet<string>();
            var result = new System.Collections.Generic.List<string>();
            foreach (System.Text.RegularExpressions.Match m in regex.Matches(methodBody))
            {
                if (seen.Add(m.Groups[1].Value)) result.Add(m.Groups[1].Value);
            }
            return result;
        }

        var clientOwner = FieldOrder(
            ExtractMethodBody(clientCode, "ApplyOwnerSnapshot(ref SpanReader reader)"));
        var serverOwner = FieldOrder(
            ExtractMethodBody(baseCode, "SerializeForOwnerClient(ref SpanWriter writer)"));
        Assert.Equal(serverOwner, clientOwner);

        var clientOther = FieldOrder(
            ExtractMethodBody(clientCode, "ApplyOtherSnapshot(ref SpanReader reader)"));
        var serverOther = FieldOrder(
            ExtractMethodBody(baseCode, "SerializeForOtherClients(ref SpanWriter writer)"));
        Assert.Equal(serverOther, clientOther);
    }

    // =========================================================================
    // Phase B1 — client ApplyReplicatedDelta emission
    //
    // Companion to Phase B0: the server's SerializeOwnerDelta /
    // SerializeOtherDelta family writes a scope-masked bitmap + changed
    // field values; the client must read that format and fire
    // OnXxxChanged for every field the bitmap marks. Unlike Apply*Snapshot
    // which is authoritative-reset / callback-silent, Apply*Delta is an
    // observed incremental change and DOES fire callbacks — matching
    // BigWorld's shouldUseCallback=true path.
    // =========================================================================

    [Fact]
    public void ClientContext_EmitsApplyReplicatedDelta_WithFlagReadAndCallbacks()
    {
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");

        var delta = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.DeltaSync"));
        Assert.NotNull(delta);
        var code = delta!.GetText().ToString();

        var body = ExtractMethodBody(code, "ApplyReplicatedDelta(ref SpanReader reader)");

        // Flags read width matches the dirty-flag backing type chosen by
        // ReplicatedDirtyFlags. AvatarDef has 5 replicable props ≤ 8,
        // so backing type is byte → reader.ReadByte().
        Assert.Contains("reader.ReadByte()", body);

        // Every replicable field is present with guard + read + callback.
        // Replicable scopes in AvatarDef: hp, modelId, mana, secret, level.
        string[] replicableProps = { "Hp", "ModelId", "Mana", "Secret", "Level" };
        foreach (var prop in replicableProps)
        {
            var fieldName = "_" + char.ToLowerInvariant(prop[0]) + prop.Substring(1);
            Assert.Contains($"flags & ReplicatedDirtyFlags.{prop}", body);
            Assert.Contains($"var old{prop} = {fieldName};", body);
            Assert.Contains($"{fieldName} = reader.", body);
            Assert.Contains($"On{prop}Changed(old{prop}, {fieldName});", body);
        }

        // Non-replicable fields (gold=Base, aiState=CellPublic, pos=CellPrivate)
        // have no entry in ApplyReplicatedDelta — the flags enum omits them.
        Assert.DoesNotContain("_gold",    body);
        Assert.DoesNotContain("_aiState", body);
        Assert.DoesNotContain("_pos ",    body);
    }

    [Fact]
    public void ClientContext_ReplicatedDirtyFlags_EnumEmittedWithoutBackingField()
    {
        // B1 decouples enum-emission from dirty-field emission. On the
        // client the enum exists (ApplyReplicatedDelta needs it to test
        // bits) but `_dirtyFlags` and IsDirty/ClearDirty are server-only.
        var source = @"
using Atlas.Client;
namespace Test;

[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");
        var props = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.Properties")).GetText().ToString();

        Assert.Contains("private enum ReplicatedDirtyFlags", props);
        Assert.DoesNotContain("_dirtyFlags", props);
        Assert.DoesNotContain("IsDirty", props);
        Assert.DoesNotContain("ClearDirty", props);
    }

    [Fact]
    public void ClientContext_DeltaFlagType_MatchesPropCount()
    {
        // Backing type picker (GetFlagsTypeInfo) picks byte/ushort/uint/ulong
        // based on replicable-prop count. A 9-field replicable set crosses
        // into ushort territory; the client decoder must track that so its
        // ReadByte/ReadUInt16/… call matches the server's WriteByte/… call
        // byte-for-byte.
        var nineDef = @"<entity name=""Bulk"">
  <properties>
    <property name=""a1"" type=""int32"" scope=""all_clients"" />
    <property name=""a2"" type=""int32"" scope=""all_clients"" />
    <property name=""a3"" type=""int32"" scope=""all_clients"" />
    <property name=""a4"" type=""int32"" scope=""all_clients"" />
    <property name=""a5"" type=""int32"" scope=""all_clients"" />
    <property name=""a6"" type=""int32"" scope=""all_clients"" />
    <property name=""a7"" type=""int32"" scope=""all_clients"" />
    <property name=""a8"" type=""int32"" scope=""all_clients"" />
    <property name=""a9"" type=""int32"" scope=""all_clients"" />
  </properties>
</entity>";
        var source = @"
using Atlas.Client;
[Atlas.Entity.Entity(""Bulk"")]
public partial class Bulk : ClientEntity
{
    public override string TypeName => ""Bulk"";
}
";
        var (result, _) = RunDefGenerator(source, nineDef, "ATLAS_CLIENT");
        var code = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Bulk.DeltaSync")).GetText().ToString();
        var body = ExtractMethodBody(code, "ApplyReplicatedDelta(ref SpanReader reader)");

        Assert.Contains("reader.ReadUInt16()", body);
        Assert.DoesNotContain("reader.ReadByte()", body);
    }

    [Fact]
    public void ClientContext_DeltaReadOrder_MirrorsServerDeltaWriteOrder()
    {
        // Full round-trip symmetry: for every replicable prop, the server's
        // SerializeReplicatedDelta writes in declaration order, and the
        // client's ApplyReplicatedDelta reads in the same order. The guard
        // is `(flags & ReplicatedDirtyFlags.Prop) != 0` on both sides, so
        // the reader position stays in lockstep with writer position.
        var clientSource = @"
using Atlas.Client;
[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var baseSource = @"
using Atlas.Entity;
using Atlas.Serialization;
[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
    public partial void UseItem(int itemId) {}
    public partial void OnPlayerDead() {}
}
";
        var (clientResult, _) = RunDefGenerator(clientSource, AvatarDef, "ATLAS_CLIENT");
        var (baseResult,   _) = RunDefGenerator(baseSource,   AvatarDef, "ATLAS_BASE");

        var clientCode = clientResult.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync")).GetText().ToString();
        var baseCode = baseResult.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync")).GetText().ToString();

        // Property-name sequence on both sides.
        var clientSeq = new System.Collections.Generic.List<string>();
        foreach (System.Text.RegularExpressions.Match m
            in System.Text.RegularExpressions.Regex.Matches(
                ExtractMethodBody(clientCode, "ApplyReplicatedDelta(ref SpanReader reader)"),
                @"ReplicatedDirtyFlags\.(\w+)"))
        {
            clientSeq.Add(m.Groups[1].Value);
        }
        var serverSeq = new System.Collections.Generic.List<string>();
        foreach (System.Text.RegularExpressions.Match m
            in System.Text.RegularExpressions.Regex.Matches(
                ExtractMethodBody(baseCode, "SerializeReplicatedDelta(ref SpanWriter writer)"),
                @"ReplicatedDirtyFlags\.(\w+)"))
        {
            serverSeq.Add(m.Groups[1].Value);
        }
        Assert.Equal(serverSeq, clientSeq);
    }

    [Fact]
    public void ClientContext_NoOtherVisibleFields_OmitsApplyOtherSnapshot()
    {
        // When a def has no AllClients/OtherClients fields, ApplyOtherSnapshot
        // is useless (and the server won't produce an other_snapshot payload
        // to feed it). The emitter must skip it to keep generated code clean.
        var ownerOnlyDef = @"<entity name=""Avatar"">
  <properties>
    <property name=""level""  type=""int32"" scope=""own_client"" />
    <property name=""secret"" type=""string"" scope=""base_and_client"" />
  </properties>
</entity>";
        var source = @"
using Atlas.Client;
[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
}
";
        var (result, _) = RunDefGenerator(source, ownerOnlyDef, "ATLAS_CLIENT");

        var delta = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.DeltaSync"));
        Assert.NotNull(delta);
        var code = delta!.GetText().ToString();

        Assert.Contains("ApplyOwnerSnapshot", code);
        Assert.DoesNotContain("ApplyOtherSnapshot", code);
    }

    [Fact]
    public void MismatchedEntityName_ReportsDEF001()
    {
        var source = @"
using Atlas.Entity;
using Atlas.Serialization;
[Entity(""NonExistent"")]
public partial class Foo : ServerEntity
{
    public override string TypeName => ""NonExistent"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_BASE");

        var errors = result.Diagnostics.Where(d => d.Id == "ATLAS_DEF001").ToList();
        Assert.Single(errors);
    }
}

// Helper: in-memory AdditionalText for tests
internal sealed class InMemoryAdditionalText : AdditionalText
{
    private readonly string _path;
    private readonly SourceText _text;

    public InMemoryAdditionalText(string path, SourceText text)
    {
        _path = path;
        _text = text;
    }

    public override string Path => _path;
    public override SourceText? GetText(System.Threading.CancellationToken cancellationToken = default)
        => _text;
}
