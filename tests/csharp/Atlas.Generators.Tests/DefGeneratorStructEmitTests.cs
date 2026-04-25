using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using Atlas.Generators.Def;
using Atlas.Generators.Def.Emitters;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Text;
using Xunit;

namespace Atlas.Generators.Tests;

// Covers the StructEmitter + PropertiesEmitter struct-property integration:
//   - `partial struct` bodies land in the Atlas.Def namespace with
//     Serialize / Deserialize pairs.
//   - Struct-typed entity properties get a nested MutRef accessor whose
//     field setters mark the owning entity's dirty bit.
//   - Auto-sync decision is exposed through a DEF014 Info diagnostic.
//
// We drive the generator end-to-end from .def XML + user source so a
// regression in any of DefParser → DefLinker → StructEmitter →
// PropertiesEmitter surfaces as a missing / wrong generated fragment.
public class DefGeneratorStructEmitTests
{
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
        public sbyte ReadInt8() => 0;
        public byte ReadUInt8() => 0;
        public short ReadInt16() => 0;
        public ushort ReadUInt16() => 0;
        public int ReadInt32() => 0;
        public uint ReadUInt32() => 0;
        public long ReadInt64() => 0;
        public ulong ReadUInt64() => 0;
        public float ReadFloat() => 0;
        public double ReadDouble() => 0;
        public string ReadString() => """";
        public byte[] ReadBytes() => System.Array.Empty<byte>();
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
                                                string preprocessorSymbol = "")
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

        var additionalTexts = new AdditionalText[]
        {
            new InMemoryAdditionalText("Avatar.def", SourceText.From(defXml, Encoding.UTF8))
        };

        var driver = CSharpGeneratorDriver.Create(
            generators: new[] { new DefGenerator().AsSourceGenerator() },
            additionalTexts: additionalTexts,
            parseOptions: parseOptions);

        return driver.RunGenerators(compilation).GetRunResult();
    }

    private static string? FindGenerated(GeneratorDriverRunResult result, string fragment) =>
        result.GeneratedTrees.FirstOrDefault(t => t.FilePath.Contains(fragment))?.GetText().ToString();

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
    // StructEmitter
    // =========================================================================

    [Fact]
    public void StructEmitter_EmitsPartialStructWithFields()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack"">
      <field name=""id"" type=""int32"" />
      <field name=""count"" type=""uint16"" />
      <field name=""bound"" type=""bool"" />
    </struct>
  </types>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "ItemStack.Struct");
        Assert.NotNull(code);

        // Namespace must be Atlas.Def so PropertiesEmitter's `using
        // Atlas.Def;` at the top of generated properties resolves.
        Assert.Contains("namespace Atlas.Def", code);
        Assert.Contains("public partial struct ItemStack", code);
        Assert.Contains("public int Id;", code);
        Assert.Contains("public ushort Count;", code);
        Assert.Contains("public bool Bound;", code);
    }

    [Fact]
    public void StructEmitter_EmitsSerializeAndDeserializePair()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack"">
      <field name=""id"" type=""int32"" />
      <field name=""count"" type=""uint16"" />
    </struct>
  </types>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "ItemStack.Struct")!;

        // Serialize walks fields in declaration order — the same order
        // DefLinker uses for wire struct_field_set ops later.
        var serializeBody = ExtractMethodBody(code, "Serialize");
        Assert.NotNull(serializeBody);
        Assert.True(serializeBody!.IndexOf("WriteInt32(Id)", System.StringComparison.Ordinal)
                    < serializeBody.IndexOf("WriteUInt16(Count)", System.StringComparison.Ordinal),
                    "field serialization must follow declaration order");

        var deserializeBody = ExtractMethodBody(code, "Deserialize");
        Assert.NotNull(deserializeBody);
        Assert.Contains("new ItemStack()", deserializeBody!);
        // The recursive element codec binds each read to a fresh local
        // before assigning to the field — so the assertion looks for the
        // Read call + the assignment separately, but on adjacent lines.
        Assert.Contains("r.ReadInt32()", deserializeBody);
        Assert.Contains("v.Id =", deserializeBody);
        Assert.Contains("r.ReadUInt16()", deserializeBody);
        Assert.Contains("v.Count =", deserializeBody);
        Assert.Contains("return v;", deserializeBody);
    }

    // =========================================================================
    // Auto-sync heuristic — unit tests of the decision function
    // =========================================================================

    private static StructDefModel MakeStruct(string name, params (string Name, PropertyDataKind Kind)[] fields)
    {
        var s = new StructDefModel { Name = name };
        foreach (var (n, k) in fields)
            s.Fields.Add(new FieldDefModel { Name = n, Type = new DataTypeRefModel { Kind = k } });
        return s;
    }

    [Fact]
    public void AutoSync_FewFieldsStaysWhole()
    {
        var s = MakeStruct("Small",
            ("a", PropertyDataKind.Int32),
            ("b", PropertyDataKind.Int32));
        var d = StructEmitter.DecideSyncMode(s);
        Assert.Equal(StructSyncMode.Whole, d.Mode);
    }

    [Fact]
    public void AutoSync_SmallByteCountStaysWhole()
    {
        // Five `bool` fields → 5 bytes → still rule 2 (≤ 8 B).
        var s = MakeStruct("Tiny",
            ("a", PropertyDataKind.Bool),
            ("b", PropertyDataKind.Bool),
            ("c", PropertyDataKind.Bool),
            ("d", PropertyDataKind.Bool),
            ("e", PropertyDataKind.Bool));
        var d = StructEmitter.DecideSyncMode(s);
        Assert.Equal(StructSyncMode.Whole, d.Mode);
    }

    [Fact]
    public void AutoSync_LargeAndManyFieldsPicksField()
    {
        // Eight int32 = 32 B exactly, fields = 8 → rule 4 kicks in.
        var s = MakeStruct("Stats",
            ("a", PropertyDataKind.Int32), ("b", PropertyDataKind.Int32),
            ("c", PropertyDataKind.Int32), ("d", PropertyDataKind.Int32),
            ("e", PropertyDataKind.Int32), ("f", PropertyDataKind.Int32),
            ("g", PropertyDataKind.Int32), ("h", PropertyDataKind.Int32));
        var d = StructEmitter.DecideSyncMode(s);
        Assert.Equal(StructSyncMode.Field, d.Mode);
    }

    [Fact]
    public void AutoSync_VariableFieldForcesWhole()
    {
        // Having a string field (variable width) overrides the size-based
        // path — the current codec can't field-sync variable-width data.
        var s = MakeStruct("Named",
            ("name", PropertyDataKind.String),
            ("a", PropertyDataKind.Int32), ("b", PropertyDataKind.Int32),
            ("c", PropertyDataKind.Int32), ("d", PropertyDataKind.Int32),
            ("e", PropertyDataKind.Int32), ("f", PropertyDataKind.Int32),
            ("g", PropertyDataKind.Int32), ("h", PropertyDataKind.Int32));
        var d = StructEmitter.DecideSyncMode(s);
        Assert.Equal(StructSyncMode.Whole, d.Mode);
        Assert.True(d.HasVariable);
    }

    [Fact]
    public void AutoSync_ExplicitOverrideHonoured()
    {
        var whole = MakeStruct("Any", ("a", PropertyDataKind.Int32));
        whole.SyncMode = StructSyncMode.Whole;
        Assert.Equal(StructSyncMode.Whole, StructEmitter.DecideSyncMode(whole).Mode);

        var field = MakeStruct("Any", ("a", PropertyDataKind.Int32));
        field.SyncMode = StructSyncMode.Field;
        Assert.Equal(StructSyncMode.Field, StructEmitter.DecideSyncMode(field).Mode);
    }

    [Fact]
    public void AutoSync_DecisionSurfacedAsInfoDiagnostic()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack"">
      <field name=""id"" type=""int32"" />
      <field name=""count"" type=""uint16"" />
    </struct>
  </types>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var diag = result.Diagnostics.FirstOrDefault(d => d.Id == "ATLAS_DEF014");
        Assert.NotNull(diag);
        Assert.Equal(DiagnosticSeverity.Info, diag!.Severity);
        Assert.Contains("ItemStack", diag.GetMessage());
        Assert.Contains("whole", diag.GetMessage());
    }

    // =========================================================================
    // PropertiesEmitter — struct-typed property MutRef
    // =========================================================================

    [Fact]
    public void StructProperty_EmitsMutRefAccessor()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack"">
      <field name=""id"" type=""int32"" />
      <field name=""count"" type=""uint16"" />
    </struct>
  </types>
  <properties>
    <property name=""mainWeapon"" type=""ItemStack"" scope=""all_clients"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "Avatar.Properties");
        Assert.NotNull(code);

        // MutRef accessor + snapshot property + nested class body.
        // The MutRef is a sealed class — `avatar.MainWeapon.Count = 5`
        // would trip CS1612 if it were a struct return.
        Assert.Contains("public MainWeaponMutRef MainWeapon", code);
        Assert.Contains("public ItemStack MainWeaponValue", code);
        Assert.Contains("public sealed class MainWeaponMutRef", code);

        // Field-level setters for each struct field — the whole point of
        // the Mutator is to enable `avatar.MainWeapon.Count = 5`.
        Assert.Contains("public int Id", code);
        Assert.Contains("public ushort Count", code);
        Assert.Contains("_owner._mainWeapon.Id", code);
        Assert.Contains("_owner._mainWeapon.Count", code);

        // Dirty bit tripped on any field write (server-side replicable).
        Assert.Contains("_owner._dirtyFlags |= ReplicatedDirtyFlags.MainWeapon", code);

        // Implicit conversion gives scripts a snapshot path:
        //   ItemStack snap = avatar.MainWeapon;
        Assert.Contains("implicit operator ItemStack", code);
    }

    [Fact]
    public void StructProperty_FieldSetterShortCircuitsOnEqualValue()
    {
        // Redundant writes must not churn the dirty bit — the generated
        // setter short-circuits on equality. Without this a script loop
        // of `avatar.MainWeapon.Count = cachedValue` would keep the
        // property dirty forever and spam downstream deltas.
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
  </types>
  <properties>
    <property name=""mainWeapon"" type=""ItemStack"" scope=""all_clients"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "Avatar.Properties")!;
        Assert.Contains("if (_owner._mainWeapon.Id == value) return;", code);
    }

    [Fact]
    public void ScalarProperty_KeepsLegacyDirtyTrackingPath()
    {
        // Adding the struct path must not touch the scalar path: plain
        // scalars keep the "old/new + OnChanged" shape — the whole unit-test
        // guard for property emission.
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp"" type=""int32"" scope=""all_clients"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "Avatar.Properties")!;
        Assert.DoesNotContain("MutRef", code);
        Assert.Contains("public int Hp", code);
        Assert.Contains("_dirtyFlags |= ReplicatedDirtyFlags.Hp", code);
        Assert.Contains("OnHpChanged(old, value)", code);
    }

    [Fact]
    public void StructProperty_ClientSideSkipsDirtyAssignment()
    {
        // Client-side entities don't own dirty tracking — writes that
        // happen there must not touch a nonexistent _dirtyFlags field.
        // Regression guard: an earlier emitter shape had a single Mutator
        // that always wrote `_dirtyFlags |= ...`, which failed to compile
        // on the client-side Properties.g.cs. The split between dirty-aware
        // (server) and dirty-free (client) emit paths fixes that.
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
  </types>
  <properties>
    <property name=""mainWeapon"" type=""ItemStack"" scope=""all_clients"" />
  </properties>
</entity>";
        var userSource = @"
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Avatar"")]
public partial class Avatar : Atlas.Client.ClientEntity
{
    public override string TypeName => ""Avatar"";
}
";
        var result = Run(userSource, xml, preprocessorSymbol: "ATLAS_CLIENT");
        var code = FindGenerated(result, "Avatar.Properties");
        // Client still emits the MutRef surface (scripts need symmetry);
        // it just must not write _dirtyFlags, because no such field
        // exists on the client-side projection.
        if (code is not null)
        {
            Assert.DoesNotContain("_dirtyFlags", code);
        }
    }

    // =========================================================================
    // Property codec routing — struct fields must go through the generated
    // struct's Serialize / Deserialize pair rather than the flat scalar
    // DefTypeHelper.WriteMethod default. The regression this guards
    // against: `WriteInt32(_mainWeapon)` was emitted silently for struct
    // props and corrupted the wire bytes.
    // =========================================================================

    [Fact]
    public void DeltaSerializer_StructPropertyUsesStructSerialize()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
  </types>
  <properties>
    <property name=""mainWeapon"" type=""ItemStack"" scope=""all_clients"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "Avatar.DeltaSync")!;
        Assert.Contains("_mainWeapon.Serialize(ref writer)", code);
        // The accidental fallback path we want to keep out of the
        // generator forever:
        Assert.DoesNotContain("writer.WriteInt32(_mainWeapon)", code);
    }

    [Fact]
    public void DeltaApply_StructPropertyUsesStructDeserialize()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
  </types>
  <properties>
    <property name=""mainWeapon"" type=""ItemStack"" scope=""all_clients"" />
  </properties>
</entity>";
        var userSource = @"
using Atlas.Entity;
using Atlas.Serialization;
namespace Test;

[Entity(""Avatar"")]
public partial class Avatar : Atlas.Client.ClientEntity
{
    public override string TypeName => ""Avatar"";
}
";
        var result = Run(userSource, xml, preprocessorSymbol: "ATLAS_CLIENT");
        var code = FindGenerated(result, "Avatar.DeltaSync")!;
        Assert.Contains("ItemStack.Deserialize(ref reader)", code);
    }

    [Fact]
    public void SerializationEmitter_StructPropertyRoundTrips()
    {
        // Full-state Serialize / Deserialize on server must also route
        // struct props through the struct's own codec — the persistence
        // (DBApp) and offload paths go through Serialize, so getting this
        // wrong would corrupt saved game state.
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
  </types>
  <properties>
    <property name=""mainWeapon"" type=""ItemStack"" scope=""all_clients"" persistent=""true"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "Avatar.Serialization")!;
        Assert.Contains("_mainWeapon.Serialize(ref bodyWriter)", code);
        Assert.Contains("_mainWeapon = ItemStack.Deserialize(ref reader)", code);
    }

    [Fact]
    public void OwnerSnapshot_StructPropertyUsesStructSerialize()
    {
        // SerializeForOwnerClient / SerializeForOtherClients land in
        // baseline packets (msg 2019) and AoI-enter envelopes — same
        // codec rules must apply there too.
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
  </types>
  <properties>
    <property name=""mainWeapon"" type=""ItemStack"" scope=""own_client"" />
  </properties>
</entity>";
        var result = Run(MinimalUserSource, xml);
        var code = FindGenerated(result, "Avatar.DeltaSync")!;
        // SerializeForOwnerClient is the scope-specific writer; its body
        // must route struct props the same way SerializeOwnerDelta does.
        Assert.Contains("SerializeForOwnerClient", code);
        Assert.Contains("_mainWeapon.Serialize(ref writer)", code);
    }

    // =========================================================================
    // Helpers
    // =========================================================================

    // Extracts the braces body of a named method. Naive but sufficient
    // for these generated sources (one nesting level).
    private static string? ExtractMethodBody(string source, string methodName)
    {
        var m = Regex.Match(source, $@"{methodName}\s*\([^)]*\)\s*\{{(?<body>[^}}]*)\}}");
        return m.Success ? m.Groups["body"].Value : null;
    }
}
