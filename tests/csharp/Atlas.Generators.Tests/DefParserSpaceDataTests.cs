using System.Collections.Generic;
using System.Linq;
using Atlas.Generators.Def;
using Atlas.Generators.Def.Emitters;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;
using Xunit;

namespace Atlas.Generators.Tests;

public class DefParserSpaceDataTests
{
    private static ParsedDef? Parse(string xml, out List<Diagnostic> diags)
    {
        diags = new List<Diagnostic>();
        return DefParser.ParseAny(SourceText.From(xml), "test.def", diags.Add);
    }

    [Fact]
    public void SpaceData_RootElementIsRecognized()
    {
        var parsed = Parse(@"<space_data>
  <key name=""npcCount"" id=""1"" type=""int32"" />
</space_data>", out var diags);
        Assert.NotNull(parsed);
        Assert.Null(parsed!.Entity);
        Assert.Null(parsed.StandaloneComponent);
        Assert.NotNull(parsed.SpaceData);
        Assert.Empty(diags);
    }

    [Fact]
    public void SpaceData_ParsesKeysWithIdAndType()
    {
        var parsed = Parse(@"<space_data>
  <key name=""npcCount"" id=""1"" type=""int32"" />
  <key name=""worldBuff"" id=""2"" type=""string"" />
</space_data>", out var diags)!;
        Assert.Empty(diags);
        Assert.Equal(2, parsed.SpaceData!.Keys.Count);

        var npc = parsed.SpaceData.Keys.First(k => k.Name == "npcCount");
        Assert.Equal((ushort)1, npc.KeyId);
        Assert.Equal(PropertyDataKind.Int32, npc.Type.Kind);

        var buff = parsed.SpaceData.Keys.First(k => k.Name == "worldBuff");
        Assert.Equal((ushort)2, buff.KeyId);
        Assert.Equal(PropertyDataKind.String, buff.Type.Kind);
    }

    [Fact]
    public void SpaceData_RejectsMissingId()
    {
        var parsed = Parse(@"<space_data>
  <key name=""x"" type=""int32"" />
</space_data>", out var diags);
        Assert.Null(parsed);
        Assert.NotEmpty(diags);
    }

    [Fact]
    public void SpaceData_RejectsIdOutOfRange()
    {
        var parsed = Parse(@"<space_data>
  <key name=""x"" id=""0"" type=""int32"" />
</space_data>", out _);
        Assert.Null(parsed);
    }

    [Fact]
    public void SpaceData_RejectsDuplicateNameOrId()
    {
        var parsedDupName = Parse(@"<space_data>
  <key name=""x"" id=""1"" type=""int32"" />
  <key name=""x"" id=""2"" type=""int32"" />
</space_data>", out _);
        Assert.Null(parsedDupName);

        var parsedDupId = Parse(@"<space_data>
  <key name=""a"" id=""1"" type=""int32"" />
  <key name=""b"" id=""1"" type=""int32"" />
</space_data>", out _);
        Assert.Null(parsedDupId);
    }

    [Fact]
    public void SpaceData_DeprecatedFlagPropagates()
    {
        var parsed = Parse(@"<space_data>
  <key name=""old"" id=""1"" type=""int32"" deprecated=""true"" />
  <key name=""npcCount"" id=""2"" type=""int32"" />
</space_data>", out var diags)!;
        Assert.Empty(diags);
        var old = parsed.SpaceData!.Keys.First(k => k.Name == "old");
        Assert.True(old.Deprecated);
        var live = parsed.SpaceData.Keys.First(k => k.Name == "npcCount");
        Assert.False(live.Deprecated);
    }

    [Fact]
    public void SpaceData_RootMustBeEntityOrComponentOrSpaceData()
    {
        var parsed = Parse(@"<bogus />", out var diags);
        Assert.Null(parsed);
        Assert.NotEmpty(diags);
    }

    [Fact]
    public void SpaceDataEmitter_GeneratesConstAndLookups()
    {
        var keys = new List<SpaceDataKeyDefModel>
        {
            new() { Name = "npcCount", KeyId = 1,
                    Type = new DataTypeRefModel { Kind = PropertyDataKind.Int32 } },
            new() { Name = "worldBuff", KeyId = 5,
                    Type = new DataTypeRefModel { Kind = PropertyDataKind.String } },
        };
        var src = SpaceDataEmitter.Emit(keys);
        Assert.Contains("public const ushort NpcCount = 1;", src);
        Assert.Contains("public const ushort WorldBuff = 5;", src);
        Assert.Contains("1 => \"npcCount\"", src);
        Assert.Contains("SpaceDataKind.Int32", src);
        Assert.Contains("SpaceDataKind.String", src);
        Assert.Contains("namespace Atlas.Space;", src);
    }

    [Fact]
    public void SpaceDataEmitter_SkipsDeprecatedEntries()
    {
        var keys = new List<SpaceDataKeyDefModel>
        {
            new() { Name = "old", KeyId = 1, Deprecated = true,
                    Type = new DataTypeRefModel { Kind = PropertyDataKind.Int32 } },
            new() { Name = "active", KeyId = 2,
                    Type = new DataTypeRefModel { Kind = PropertyDataKind.Int32 } },
        };
        var src = SpaceDataEmitter.Emit(keys);
        Assert.DoesNotContain("public const ushort Old", src);
        Assert.Contains("public const ushort Active = 2;", src);
    }

    [Fact]
    public void SpaceDataEmitter_SortsByKeyIdForDeterministicOutput()
    {
        var keys = new List<SpaceDataKeyDefModel>
        {
            new() { Name = "c", KeyId = 30,
                    Type = new DataTypeRefModel { Kind = PropertyDataKind.Int32 } },
            new() { Name = "a", KeyId = 10,
                    Type = new DataTypeRefModel { Kind = PropertyDataKind.Int32 } },
            new() { Name = "b", KeyId = 20,
                    Type = new DataTypeRefModel { Kind = PropertyDataKind.Int32 } },
        };
        var src = SpaceDataEmitter.Emit(keys);
        var posA = src.IndexOf("= 10;");
        var posB = src.IndexOf("= 20;");
        var posC = src.IndexOf("= 30;");
        Assert.True(posA < posB);
        Assert.True(posB < posC);
    }
}
