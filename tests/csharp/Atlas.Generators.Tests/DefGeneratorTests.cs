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
    public enum RpcTarget : byte { Owner = 0, Others = 1, All = 2 }
}
namespace Atlas.Entity
{
    public abstract class ServerEntity
    {
        public uint EntityId { get; internal set; }
        public abstract string TypeName { get; }
        protected internal void SendClientRpc(int rpcId, Atlas.Core.RpcTarget target, System.ReadOnlySpan<byte> payload) { }
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
        var manifestText = SourceText.From(TestManifest.Derive(defXml), System.Text.Encoding.UTF8);
        var additionalTexts = new AdditionalText[]
        {
            new InMemoryAdditionalText("Avatar.def", defText),
            new InMemoryAdditionalText("entity_ids.xml", manifestText),
        };

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
    public void BaseContext_ReplyMethod_MailboxReturnsAtlasTaskRpcReply()
    {
        const string defXml = @"<entity name=""Avatar"">
  <base_methods>
    <method name=""GetLevel"" reply=""int32"" />
    <method name=""LookupName"" reply=""string"">
      <arg name=""dbid"" type=""int64"" />
    </method>
  </base_methods>
</entity>";
        const string source = @"
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
    public partial AtlasTask<RpcReply<int>> GetLevel() => RpcReply<int>.Ok(0);
    public partial AtlasTask<RpcReply<string>> LookupName(long dbid) => RpcReply<string>.Ok("""");
}
";
        var (result, _) = RunDefGenerator(source, defXml, "ATLAS_CELL");
        var mailboxes = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.Mailboxes"));
        Assert.NotNull(mailboxes);
        var code = mailboxes!.GetText().ToString();

        Assert.Contains("public AtlasTask<RpcReply<int>> GetLevel(", code);
        Assert.Contains("public AtlasTask<RpcReply<string>> LookupName(long dbid", code);
        Assert.Contains("AtlasCancellationToken ct = default", code);
        Assert.Contains("MessageIds.EntityRpcReply", code);
        Assert.Contains("RpcReplyHelpers.For<int>()", code);
        Assert.Contains("RpcReplyHelpers.For<string>()", code);
        Assert.Contains("AtlasRpcSource<RpcReply<int>>.Rent()", code);
        // request_id header is the first thing written into the request body.
        Assert.Contains("writer.WriteUInt32(requestId);", code);
        // Reply bit must be OR'd into rpc_id (high nibble has 8 in it).
        Assert.Contains("unchecked((int)0x", code);
    }

    [Fact]
    public void BaseContext_ReplyMethod_DispatcherInvokesAndForwardsReply()
    {
        const string defXml = @"<entity name=""Avatar"">
  <base_methods>
    <method name=""GetLevel"" reply=""int32"" />
  </base_methods>
</entity>";
        const string source = @"
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
    public partial AtlasTask<RpcReply<int>> GetLevel() => RpcReply<int>.Ok(0);
}
";
        var (result, _) = RunDefGenerator(source, defXml, "ATLAS_BASE");
        var dispatcher = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("DefRpcDispatcher"));
        Assert.NotNull(dispatcher);
        var code = dispatcher!.GetText().ToString();

        Assert.Contains("IntPtr replyChannel", code);
        Assert.Contains("uint requestId = reader.ReadUInt32();", code);
        Assert.Contains("var __task = target.GetLevel(", code);
        Assert.Contains("EntityRpcReplyHelpers.SendReplyOnComplete(__task, replyChannel, requestId,", code);
        // Reply serializer is emitted as a static field — closure-free.
        Assert.Contains("EntityRpcReplyHelpers.ReplySerializer<int> s_Avatar_GetLevelReplySerializer", code);
    }

    [Fact]
    public void BaseContext_ReplyMethod_RpcStubEmitsPartialDeclaration()
    {
        const string defXml = @"<entity name=""Avatar"">
  <base_methods>
    <method name=""GetLevel"" reply=""int32"" />
  </base_methods>
</entity>";
        const string source = @"
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
    public partial AtlasTask<RpcReply<int>> GetLevel() => RpcReply<int>.Ok(0);
}
";
        var (result, _) = RunDefGenerator(source, defXml, "ATLAS_BASE");
        var stubs = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("Avatar.RpcStubs"));
        Assert.NotNull(stubs);
        var code = stubs!.GetText().ToString();

        Assert.Contains("public partial AtlasTask<RpcReply<int>> GetLevel();", code);
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
        Assert.Contains("public AvatarClientMailbox Client => new(this, RpcTarget.Owner);", code);
        Assert.Contains("public AvatarClientMailbox OtherClients => new(this, RpcTarget.Others);", code);
        Assert.Contains("public AvatarClientMailbox AllClients => new(this, RpcTarget.All);", code);
        Assert.Contains("internal AvatarClientMailbox(Avatar entity, RpcTarget target)", code);
        Assert.Contains("_entity.InternalSendClientRpc(0x", code);
        Assert.Contains("_target, writer.WrittenSpan", code);
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

    // Retired tests (kept as a pointer for anyone chasing git blame):
    //   - DeltaSync_EmitsReliableAndUnreliableMethods
    //   - DeltaSync_AllUnreliable_EmitsEmptyReliableMask
    // Both asserted on the now-deleted SerializeReplicatedDelta{,Reliable,
    // Unreliable} + HasReliableDirty / HasUnreliableDirty +
    // ReliableDirtyMask / UnreliableDirtyMask emitter surface. The per-
    // channel reliability split moved to the transport layer
    // (ReplicatedReliableDeltaFromCell vs ReplicatedDeltaFromCell); the
    // serializer now only has the audience split
    // (SerializeOwnerDelta vs SerializeOtherDelta).

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
        // hp/level all_clients/own_client → cell-side under M2.
        var (result, _) = RunDefGenerator(source, xml, "ATLAS_CELL");

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

        // ApplyOwnerSnapshot mirrors the CELL-side SerializeForOwnerClient
        // only — base-scope own-client props (secret) flow on a separate
        // wire (BigWorld model: base sends set_<propname>, cell sends
        // createCellPlayer). The owner-visible cell-scope fields in
        // declaration order are: hp, mana, level.
        var body = ExtractMethodBody(code, "ApplyOwnerSnapshot(ref SpanReader reader)");
        int hp    = body.IndexOf("_hp = reader.ReadInt32()",   System.StringComparison.Ordinal);
        int mana  = body.IndexOf("_mana = reader.ReadInt32()", System.StringComparison.Ordinal);
        int level = body.IndexOf("_level = reader.ReadInt32()",System.StringComparison.Ordinal);

        Assert.True(hp >= 0 && mana > hp && level > mana,
            $"owner-visible cell-scope fields not in declaration order; body was:\n{body}");

        // Fields outside the cell owner-scope must not appear here.
        Assert.DoesNotContain("_gold",    body);  // scope=base (base-only)
        Assert.DoesNotContain("_secret",  body);  // scope=base_and_client — lives on base wire
        Assert.DoesNotContain("_aiState", body);  // scope=cell_public (no client visibility)
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
        // Compare against cell-side emission — M2 moved the cell-scope
        // replicable props (hp/aiState/modelId/mana/level) off the base
        // partial, so the client's owner/other snapshot wire is mirror
        // symmetric with the CELL serializer, not the base one.
        // Base-side projection (secret base_and_client) is a separate
        // lockstep covered by its own test (see
        // ClientContext_BaseAndClientProperty_*).
        var (clientResult, _) = RunDefGenerator(clientSource, AvatarDef, "ATLAS_CLIENT");
        var (baseResult,   _) = RunDefGenerator(baseSource,   AvatarDef, "ATLAS_CELL");

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
        // All scalar — guarded by `scalarFlags &` after the sectionMask
        // dispatch.
        string[] replicableProps = { "Hp", "ModelId", "Mana", "Secret", "Level" };
        foreach (var prop in replicableProps)
        {
            var fieldName = "_" + char.ToLowerInvariant(prop[0]) + prop.Substring(1);
            Assert.Contains($"scalarFlags & ReplicatedDirtyFlags.{prop}", body);
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
        // The flag-byte type for 9 props is ushort (ReadUInt16). The
        // sectionMask byte at the top is ReadByte though — that's a
        // fixed u8 regardless of prop count. Check that the flag bytes
        // (post-sectionMask) use ushort and not byte.
        Assert.Contains("scalarFlags = (ReplicatedDirtyFlags)reader.ReadUInt16()", body);
    }

    [Fact]
    public void ClientContext_DeltaReadOrder_MirrorsServerDeltaWriteOrder()
    {
        // Full round-trip symmetry: for every replicable prop, the server's
        // SerializeOtherDelta (post-C10) writes in declaration order, and
        // the client's ApplyReplicatedDelta reads in the same order. The
        // guard is `(flags & ReplicatedDirtyFlags.Prop) != 0` on both
        // sides, so the reader position stays in lockstep with writer
        // position.
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
        // M2: delta-write now happens per side. Client's merged
        // ApplyReplicatedDelta must agree with whichever side actually
        // produced the delta — for the cell-scope props in AvatarDef
        // (every replicable except `secret`), that's the cell side.
        var (clientResult, _) = RunDefGenerator(clientSource, AvatarDef, "ATLAS_CLIENT");
        var (baseResult,   _) = RunDefGenerator(baseSource,   AvatarDef, "ATLAS_CELL");

        var clientCode = clientResult.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync")).GetText().ToString();
        var baseCode = baseResult.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync")).GetText().ToString();

        // Property-name sequence on both sides. ReplicatedDirtyFlags.None
        // appears in section masks (e.g. `if (containerFlags != ReplicatedDirtyFlags.None)`)
        // — strip those, only the per-prop entries matter for read/write order.
        var clientSeq = new System.Collections.Generic.List<string>();
        foreach (System.Text.RegularExpressions.Match m
            in System.Text.RegularExpressions.Regex.Matches(
                ExtractMethodBody(clientCode, "ApplyReplicatedDelta(ref SpanReader reader)"),
                @"ReplicatedDirtyFlags\.(\w+)"))
        {
            if (m.Groups[1].Value == "None") continue;
            clientSeq.Add(m.Groups[1].Value);
        }
        var serverSeq = new System.Collections.Generic.List<string>();
        foreach (System.Text.RegularExpressions.Match m
            in System.Text.RegularExpressions.Regex.Matches(
                ExtractMethodBody(baseCode, "SerializeOtherDelta(ref SpanWriter writer)"),
                @"ReplicatedDirtyFlags\.(\w+)"))
        {
            if (m.Groups[1].Value == "None") continue;
            serverSeq.Add(m.Groups[1].Value);
        }
        // Under M2, each server side writes a SUBSET of the replicable
        // props (cell writes cell-scope, base writes base-scope). Client's
        // reader walks the union in declaration order; for the guard
        // `if ((flags & .Prop) != 0) reader.Read()` to stay in lockstep,
        // the server's write order must be a declaration-ordered
        // SUBSEQUENCE of the client's read order. Equality was the
        // pre-M2 invariant when a single server ctx wrote all
        // client-visible props.
        int idx = 0;
        foreach (var name in serverSeq)
        {
            while (idx < clientSeq.Count && clientSeq[idx] != name) idx++;
            Assert.True(idx < clientSeq.Count,
                $"server seq prop '{name}' not found in client seq; " +
                $"client={string.Join(",", clientSeq)} server={string.Join(",", serverSeq)}");
            idx++;
        }
    }

    // =========================================================================
    // Phase B2 — client setter stays silent (aligned with BigWorld)
    //
    // BigWorld's simple_client_entity.cpp::propertyEvent fires the
    // set_<propname> script callback ONLY on network-received changes; a
    // local Python write (`entity.hp = 60`) is completely silent. Clients
    // observe authoritative server state — local writes are edge cases
    // (prediction, test fixtures) that shouldn't masquerade as observed
    // transitions. Atlas aligns: OnXxxChanged is reachable only through
    // ApplyReplicatedDelta (wire path), never through the property setter
    // on the client side.
    // =========================================================================

    [Fact]
    public void ClientContext_ReplicableSetter_IsBareAssignment()
    {
        var source = @"
using Atlas.Client;
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

        var hpBlock = ExtractMethodBody(props, "public int Hp");
        // Setter is a single expression-bodied assignment.
        Assert.Contains("set => _hp = value;", hpBlock);
        // The inequality guard / old-capture / callback scaffolding that
        // the server setter uses must NOT appear — those belong to the
        // dirty-tracking path only.
        Assert.DoesNotContain("if (_hp != value)", hpBlock);
        Assert.DoesNotContain("var old = _hp;", hpBlock);
        Assert.DoesNotContain("OnHpChanged", hpBlock);
        Assert.DoesNotContain("_dirtyFlags", hpBlock);
    }

    [Fact]
    public void ClientContext_OnXxxChanged_ReachableOnlyFromApplyReplicatedDelta()
    {
        // Load-bearing invariant for scheme 2 (BigWorld alignment). The only
        // call site of OnHpChanged on the client must be inside
        // ApplyReplicatedDelta. If a future refactor accidentally re-adds a
        // setter-side callback, or if local script code starts firing the
        // hook, this pins it.
        var source = @"
using Atlas.Client;
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
        var delta = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync")).GetText().ToString();

        // Properties.g.cs has exactly one OnHpChanged mention — the
        // `partial void` declaration the generator always emits for scripts
        // to implement. No other references.
        var mentions = System.Text.RegularExpressions.Regex.Matches(props, @"\bOnHpChanged\b").Count;
        Assert.Equal(1, mentions);
        Assert.Contains("partial void OnHpChanged(", props);

        // DeltaSync.g.cs invokes OnHpChanged at least once, and every
        // invocation is inside ApplyReplicatedDelta.
        var applyBody = ExtractMethodBody(delta, "ApplyReplicatedDelta(ref SpanReader reader)");
        Assert.Contains("OnHpChanged(oldHp, _hp);", applyBody);
        int applyCount = System.Text.RegularExpressions.Regex.Matches(applyBody, @"\bOnHpChanged\b").Count;
        int totalCount = System.Text.RegularExpressions.Regex.Matches(delta, @"\bOnHpChanged\b").Count;
        Assert.Equal(applyCount, totalCount);
    }

    [Fact]
    public void ServerContext_SetterStillTracksDirty()
    {
        // Regression: B2 only changed the client path. The server setter
        // must still fire both dirty-bit-set AND OnXxxChanged.
        var source = @"
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
        // hp is all_clients (cell-scope) — its backing field and setter
        // live in the cell partial under M2.
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CELL");
        var props = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.Properties")).GetText().ToString();

        var hpBlock = ExtractMethodBody(props, "public int Hp");
        Assert.Contains("if (_hp != value)", hpBlock);
        Assert.Contains("_dirtyFlags |= ReplicatedDirtyFlags.Hp;", hpBlock);
        Assert.Contains("OnHpChanged(old, value);", hpBlock);
    }

    [Fact]
    public void ClientContext_DeltaStillBypassesSetter()
    {
        // Regression guard for B1: ApplyReplicatedDelta must keep writing
        // to the private backing field directly. If it started going
        // through the new-in-B2 callback-firing setter, OnXxxChanged
        // would fire twice per delta (once from setter, once from the
        // explicit call in ApplyReplicatedDelta).
        var source = @"
using Atlas.Client;
[Atlas.Entity.Entity(""Avatar"")]
public partial class Avatar : ClientEntity
{
    public override string TypeName => ""Avatar"";
    public partial void ShowDamage(int amount, uint attackerId) {}
}
";
        var (result, _) = RunDefGenerator(source, AvatarDef, "ATLAS_CLIENT");
        var code = result.GeneratedTrees
            .First(t => t.FilePath.Contains("Avatar.DeltaSync")).GetText().ToString();
        var body = ExtractMethodBody(code, "ApplyReplicatedDelta(ref SpanReader reader)");

        // The delta body assigns `_hp = reader.ReadX()` and calls
        // `OnHpChanged(oldHp, _hp)` explicitly. It must NOT go through the
        // public property (`Hp = reader.ReadX()`) which would re-trigger
        // OnHpChanged from the setter.
        Assert.Contains("_hp = reader.", body);
        Assert.DoesNotContain("Hp = reader", body);
        Assert.Contains("OnHpChanged(oldHp, _hp);", body);
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

    // =========================================================================
    // Phase B5 — ATLAS_DEF008: replicable `position` is reserved
    //
    // Position is already handled by the volatile channel
    // (kEntityPositionUpdate envelope) and the ClientEntity base class.
    // A .def that also declares a replicable property named "position"
    // would produce two sources of truth. The parser flags such a
    // property and all emitters skip it (no enum bit, no backing field,
    // no serializer / deserializer). The user gets a warning rather
    // than a silent drop.
    // =========================================================================

    [Fact]
    public void DEF008_FiresOnReplicablePosition()
    {
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""position"" type=""vector3"" scope=""all_clients"" />
  </properties>
</entity>";
        Diagnostic? reported = null;
        var model = DefParser.Parse(SourceText.From(xml, System.Text.Encoding.UTF8), "Avatar.def",
                                    d => { if (d.Id == "ATLAS_DEF008") reported = d; });

        Assert.NotNull(reported);
        Assert.Equal(DiagnosticSeverity.Warning, reported!.Severity);
        // Model still contains the property but with the skip flag set, so
        // every emitter can filter it uniformly.
        Assert.True(model!.Properties[0].IsReservedPosition);
    }

    [Fact]
    public void DEF008_CaseInsensitive()
    {
        // "position" / "Position" / "POSITION" all trigger.
        foreach (var name in new[] { "Position", "POSITION", "PoSiTiOn" })
        {
            var xml = $@"<entity name=""Avatar"">
  <properties>
    <property name=""{name}"" type=""vector3"" scope=""all_clients"" />
  </properties>
</entity>";
            Diagnostic? reported = null;
            var model = DefParser.Parse(SourceText.From(xml, System.Text.Encoding.UTF8),
                                        "Avatar.def",
                                        d => { if (d.Id == "ATLAS_DEF008") reported = d; });
            Assert.NotNull(reported);
            Assert.True(model!.Properties[0].IsReservedPosition, $"failed for name={name}");
        }
    }

    [Fact]
    public void DEF008_SkipsNonReplicablePosition()
    {
        // A non-replicable `position` (e.g. cell_private) has no wire path
        // to conflict with the volatile channel — no diagnostic, no skip.
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""position"" type=""vector3"" scope=""cell_private"" />
  </properties>
</entity>";
        Diagnostic? reported = null;
        var model = DefParser.Parse(SourceText.From(xml, System.Text.Encoding.UTF8), "Avatar.def",
                                    d => { if (d.Id == "ATLAS_DEF008") reported = d; });

        Assert.Null(reported);
        Assert.False(model!.Properties[0].IsReservedPosition);
    }

    [Fact]
    public void DEF008_DoesNotFireOnPositionPrefixOrSuffix()
    {
        // Partial matches (my_position, positions, spawnPosition) must
        // NOT trigger — only the exact name is reserved.
        foreach (var name in new[] { "spawnPosition", "positions", "my_position", "pos" })
        {
            var xml = $@"<entity name=""Avatar"">
  <properties>
    <property name=""{name}"" type=""vector3"" scope=""all_clients"" />
  </properties>
</entity>";
            Diagnostic? reported = null;
            var model = DefParser.Parse(SourceText.From(xml, System.Text.Encoding.UTF8),
                                        "Avatar.def",
                                        d => { if (d.Id == "ATLAS_DEF008") reported = d; });
            Assert.Null(reported);
            Assert.False(model!.Properties[0].IsReservedPosition, $"failed for name={name}");
        }
    }

    [Fact]
    public void DEF008_EmittersSkipReservedPositionEntirely()
    {
        // Full emitter-level skip invariant: `position` in a replicable scope
        // produces no backing field in Properties.g.cs, no enum bit in
        // ReplicatedDirtyFlags, no serialize/deserialize references
        // anywhere, no entry in the TypeRegistry blob.
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp""       type=""int32""   scope=""all_clients"" />
    <property name=""position"" type=""vector3"" scope=""all_clients"" />
  </properties>
</entity>";
        var source = @"
using Atlas.Entity;
using Atlas.Serialization;
[Entity(""Avatar"")]
public partial class Avatar : ServerEntity
{
    public override string TypeName => ""Avatar"";
    public override void Serialize(ref SpanWriter w) {}
    public override void Deserialize(ref SpanReader r) {}
}
";
        // hp is all_clients (cell-scope) under M2 — run under ATLAS_CELL so
        // the test exercises the side that actually emits the hp backing
        // field.
        var (result, _) = RunDefGenerator(source, xml, "ATLAS_CELL");

        // Diagnostic surfaced.
        Assert.Contains(result.Diagnostics, d => d.Id == "ATLAS_DEF008");

        // Combined output of all generated files: no mention of the
        // position backing field, enum flag, or script-side property.
        var allCode = string.Join("\n", result.GeneratedTrees
            .Select(t => t.GetText().ToString()));

        Assert.DoesNotContain("_position", allCode);
        Assert.DoesNotContain("ReplicatedDirtyFlags.Position", allCode);
        Assert.DoesNotContain("public Atlas.DataTypes.Vector3 Position", allCode);
        Assert.DoesNotContain("OnPositionChanged", allCode);

        // And hp (the other, valid replicable field) is intact — skipping
        // position didn't take the whole file down.
        Assert.Contains("_hp", allCode);
        Assert.Contains("ReplicatedDirtyFlags.Hp", allCode);
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
