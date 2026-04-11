using System;
using System.Linq;
using Atlas.Generators.Entity;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Xunit;

namespace Atlas.Generators.Tests;

public class EntityGeneratorTests
{
    // Helper to run the generator on a given source string
    private static (GeneratorDriverRunResult result, Compilation compilation) RunGenerator(string source)
    {
        // We need to provide attribute stubs and ServerEntity base class as source code
        // rather than referencing assemblies, since the generator only needs syntax/symbols.
        var attributeSource = @"
using System;
namespace Atlas.Entity
{
    [AttributeUsage(AttributeTargets.Class)]
    public sealed class EntityAttribute : Attribute
    {
        public string TypeName { get; }
        public EntityAttribute(string typeName) => TypeName = typeName;
    }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class ReplicatedAttribute : Attribute
    {
        public ReplicationScope Scope { get; set; } = ReplicationScope.AllClients;
    }

    public enum ReplicationScope : byte { CellPrivate = 0, BaseOnly = 1, OwnClient = 2, AllClients = 3 }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class PersistentAttribute : Attribute { }

    [AttributeUsage(AttributeTargets.Field)]
    public sealed class ServerOnlyAttribute : Attribute { }
}
";
        var serverEntitySource = @"
namespace Atlas.Entity
{
    public abstract class ServerEntity
    {
        public uint EntityId { get; internal set; }
        public bool IsDestroyed { get; internal set; }
        public abstract string TypeName { get; }
        public abstract void Serialize(ref Atlas.Serialization.SpanWriter writer);
        public abstract void Deserialize(ref Atlas.Serialization.SpanReader reader);
    }
}
namespace Atlas.Serialization
{
    public ref struct SpanWriter
    {
        public SpanWriter(int capacity) { }
        public void WriteByte(byte value) { }
        public void WriteUInt16(ushort value) { }
        public void WriteInt32(int value) { }
        public void WriteFloat(float value) { }
        public void WriteString(string value) { }
        public void WriteBool(bool value) { }
        public void WriteRawBytes(System.ReadOnlySpan<byte> data) { }
        public readonly System.ReadOnlySpan<byte> WrittenSpan => default;
        public readonly int Length => 0;
        public readonly int Position => 0;
        public void Dispose() { }
        public void WriteUInt32(uint value) { }
    }
    public ref struct SpanReader
    {
        public SpanReader(System.ReadOnlySpan<byte> data) { }
        public byte ReadByte() => 0;
        public ushort ReadUInt16() => 0;
        public int ReadInt32() => 0;
        public float ReadFloat() => 0f;
        public string ReadString() => "";
        public bool ReadBool() => false;
        public uint ReadUInt32() => 0;
        public readonly int Position => 0;
        public void Advance(int count) { }
    }
}
";

        var compilation = CSharpCompilation.Create("TestAssembly",
            new[]
            {
                CSharpSyntaxTree.ParseText(attributeSource),
                CSharpSyntaxTree.ParseText(serverEntitySource),
                CSharpSyntaxTree.ParseText(source),
            },
            new[]
            {
                MetadataReference.CreateFromFile(typeof(object).Assembly.Location),
                MetadataReference.CreateFromFile(typeof(Attribute).Assembly.Location),
            },
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));

        // Also add System.Runtime reference for netcoreapp
        var runtimeDir = System.IO.Path.GetDirectoryName(typeof(object).Assembly.Location)!;
        var systemRuntime = System.IO.Path.Combine(runtimeDir, "System.Runtime.dll");
        if (System.IO.File.Exists(systemRuntime))
            compilation = compilation.AddReferences(MetadataReference.CreateFromFile(systemRuntime));

        var generator = new EntityGenerator();
        var driver = CSharpGeneratorDriver.Create(generator);
        var runResult = driver.RunGenerators(compilation).GetRunResult();
        return (runResult, compilation);
    }

    [Fact]
    public void ValidEntity_GeneratesSerializationAndFactory()
    {
        var source = @"
using Atlas.Entity;
[Entity(""Player"")]
public partial class PlayerEntity : ServerEntity
{
    [Replicated] private int _health = 100;
    [Persistent] private string _name = """";
}
";
        var (result, _) = RunGenerator(source);

        // Should have no errors (only warnings about field naming)
        Assert.Empty(result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error));

        // Should generate serialization, dirty tracking, factory, type registry
        var generatedFiles = result.GeneratedTrees.Select(t => System.IO.Path.GetFileName(t.FilePath)).ToList();
        Assert.Contains("PlayerEntity.Serialization.g.cs", generatedFiles);
        Assert.Contains("PlayerEntity.DirtyTracking.g.cs", generatedFiles);
        Assert.Contains("EntityFactory.g.cs", generatedFiles);
        Assert.Contains("EntityTypeRegistry.g.cs", generatedFiles);
    }

    [Fact]
    public void Serialization_ContainsVersionAndFieldCount()
    {
        var source = @"
using Atlas.Entity;
[Entity(""Monster"")]
public partial class MonsterEntity : ServerEntity
{
    [Replicated] private int _hp = 50;
    [Replicated] private float _speed = 1.0f;
}
";
        var (result, _) = RunGenerator(source);
        var serializationTree = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("MonsterEntity.Serialization"));
        Assert.NotNull(serializationTree);

        var text = serializationTree!.GetText().ToString();
        Assert.Contains("kSerializationVersion", text);
        Assert.Contains("writer.WriteUInt16((ushort)2)", text); // fieldCount = 2
        Assert.Contains("bodyWriter", text); // temp buffer for bodyLength
        Assert.Contains("fieldCount >", text); // conditional reads in Deserialize
    }

    [Fact]
    public void NonPartialClass_EmitsDiagnostic()
    {
        var source = @"
using Atlas.Entity;
[Entity(""Broken"")]
public class BrokenEntity : ServerEntity
{
    [Replicated] private int _x;
    // Note: class is NOT partial
}
";
        var (result, _) = RunGenerator(source);
        var error = result.Diagnostics.FirstOrDefault(d => d.Id == "ATLAS_ENTITY001");
        Assert.NotNull(error);
        Assert.Equal(DiagnosticSeverity.Error, error!.Severity);
    }

    [Fact]
    public void MissingBaseClass_EmitsDiagnostic()
    {
        var source = @"
using Atlas.Entity;
[Entity(""NoBase"")]
public partial class NoBaseEntity
{
    [Replicated] private int _x;
}
";
        var (result, _) = RunGenerator(source);
        var error = result.Diagnostics.FirstOrDefault(d => d.Id == "ATLAS_ENTITY003");
        Assert.NotNull(error);
        Assert.Equal(DiagnosticSeverity.Error, error!.Severity);
    }

    [Fact]
    public void NonPrivateField_EmitsWarning()
    {
        var source = @"
using Atlas.Entity;
[Entity(""Warn"")]
public partial class WarnEntity : ServerEntity
{
    [Replicated] public int _health;
}
";
        var (result, _) = RunGenerator(source);
        var warning = result.Diagnostics.FirstOrDefault(d => d.Id == "ATLAS_ENTITY004");
        Assert.NotNull(warning);
        Assert.Equal(DiagnosticSeverity.Warning, warning!.Severity);
    }

    [Fact]
    public void FieldWithoutUnderscore_EmitsWarning()
    {
        var source = @"
using Atlas.Entity;
[Entity(""Warn2"")]
public partial class Warn2Entity : ServerEntity
{
    [Replicated] private int health;
}
";
        var (result, _) = RunGenerator(source);
        var warning = result.Diagnostics.FirstOrDefault(d => d.Id == "ATLAS_ENTITY005");
        Assert.NotNull(warning);
        Assert.Equal(DiagnosticSeverity.Warning, warning!.Severity);
    }

    [Fact]
    public void EmptyEntity_GeneratesFactoryAndRegistry()
    {
        // An entity with no fields should still generate factory + registry
        var source = @"
using Atlas.Entity;
[Entity(""Empty"")]
public partial class EmptyEntity : ServerEntity { }
";
        var (result, _) = RunGenerator(source);
        Assert.Empty(result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error));

        var generatedFiles = result.GeneratedTrees.Select(t => System.IO.Path.GetFileName(t.FilePath)).ToList();
        Assert.Contains("EntityFactory.g.cs", generatedFiles);

        var factoryText = result.GeneratedTrees
            .First(t => t.FilePath.Contains("EntityFactory"))
            .GetText().ToString();
        Assert.Contains("EmptyEntity", factoryText);
    }

    [Fact]
    public void DirtyTracking_GeneratesPropertiesAndFlags()
    {
        var source = @"
using Atlas.Entity;
[Entity(""Tracked"")]
public partial class TrackedEntity : ServerEntity
{
    [Replicated] private int _health;
    [Replicated] private float _mana;
    [Persistent] private string _name = """";
}
";
        var (result, _) = RunGenerator(source);
        var dirtyTree = result.GeneratedTrees
            .FirstOrDefault(t => t.FilePath.Contains("TrackedEntity.DirtyTracking"));
        Assert.NotNull(dirtyTree);

        var text = dirtyTree!.GetText().ToString();
        Assert.Contains("ReplicatedDirtyFlags", text);
        Assert.Contains("Health", text); // property for _health
        Assert.Contains("Mana", text);   // property for _mana
        Assert.Contains("IsDirty", text);
        Assert.Contains("ClearDirty", text);
        Assert.Contains("SerializeReplicatedDelta", text);
        Assert.Contains("ApplyReplicatedDelta", text);
    }

    [Fact]
    public void NoEntities_StillGeneratesEmptyFactoryAndRegistry()
    {
        // No [Entity] classes at all
        var source = @"
public class RegularClass { }
";
        var (result, _) = RunGenerator(source);
        // Factory and Registry should still be generated (empty)
        var generatedFiles = result.GeneratedTrees.Select(t => System.IO.Path.GetFileName(t.FilePath)).ToList();
        Assert.Contains("EntityFactory.g.cs", generatedFiles);
        Assert.Contains("EntityTypeRegistry.g.cs", generatedFiles);
    }
}
