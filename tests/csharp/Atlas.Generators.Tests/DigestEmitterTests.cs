using System.Collections.Generic;
using Atlas.Generators.Def;
using Atlas.Generators.Def.Emitters;
using Xunit;

namespace Atlas.Generators.Tests;

public class DigestEmitterTests
{
    private static EntityDefModel MakeAvatar()
    {
        var def = new EntityDefModel { Name = "Avatar" };
        def.Properties.Add(new PropertyDefModel
        {
            Name = "hp",
            Type = "int32",
            Scope = PropertyScope.AllClients,
        });
        def.ClientMethods.Add(new MethodDefModel { Name = "ShowEffect" });
        return def;
    }

    [Fact]
    public void Emit_SameInputs_ProducesIdenticalDigest()
    {
        var defs = new List<EntityDefModel> { MakeAvatar() };
        var typeIds = new Dictionary<string, ushort> { ["Avatar"] = 1 };

        var first = DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                       new List<ComponentDefModel>(), typeIds);
        var second = DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                        new List<ComponentDefModel>(), typeIds);

        Assert.Equal(ExtractHex(first), ExtractHex(second));
    }

    [Fact]
    public void Emit_RenameMethod_ChangesDigest()
    {
        var defs = new List<EntityDefModel> { MakeAvatar() };
        var typeIds = new Dictionary<string, ushort> { ["Avatar"] = 1 };
        var baseline = ExtractHex(DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                                     new List<ComponentDefModel>(), typeIds));

        defs[0].ClientMethods[0].Name = "ShowEffectV2";
        var mutated = ExtractHex(DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                                    new List<ComponentDefModel>(), typeIds));

        Assert.NotEqual(baseline, mutated);
    }

    [Fact]
    public void Emit_DifferentTypeId_ChangesDigest()
    {
        var defs = new List<EntityDefModel> { MakeAvatar() };
        var first = ExtractHex(DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                                  new List<ComponentDefModel>(),
                                                  new Dictionary<string, ushort> { ["Avatar"] = 1 }));
        var second = ExtractHex(DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                                   new List<ComponentDefModel>(),
                                                   new Dictionary<string, ushort> { ["Avatar"] = 2 }));
        Assert.NotEqual(first, second);
    }

    [Fact]
    public void Emit_AppendsBytesArrayLiteral()
    {
        var defs = new List<EntityDefModel> { MakeAvatar() };
        var typeIds = new Dictionary<string, ushort> { ["Avatar"] = 1 };
        var code = DigestEmitter.Emit(defs, new List<StructDefModel>(),
                                      new List<ComponentDefModel>(), typeIds);
        Assert.Contains("Sha256Hex = \"", code);
        Assert.Contains("private static readonly byte[] s_bytes", code);
        Assert.Contains("public static System.ReadOnlySpan<byte> Bytes => s_bytes;", code);
    }

    private static string ExtractHex(string emitted)
    {
        const string Marker = "Sha256Hex = \"";
        int start = emitted.IndexOf(Marker, System.StringComparison.Ordinal);
        Assert.True(start >= 0);
        start += Marker.Length;
        int end = emitted.IndexOf('"', start);
        return emitted.Substring(start, end - start);
    }
}
