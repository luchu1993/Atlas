using System.Collections.Generic;
using Atlas.Generators.Def;
using Microsoft.CodeAnalysis;
using Xunit;

namespace Atlas.Generators.Tests;

public class EntityIdManifestTests
{
    private static List<Diagnostic> Capture(out System.Action<Diagnostic> sink)
    {
        var captured = new List<Diagnostic>();
        sink = captured.Add;
        return captured;
    }

    [Fact]
    public void Parse_ValidManifest_ReturnsEntries()
    {
        var xml = @"<entity_ids>
  <entity name=""Avatar"" id=""1""/>
  <entity name=""Account"" id=""2""/>
  <entity name=""_removed"" id=""3"" deprecated=""true""/>
</entity_ids>";

        var diags = Capture(out var sink);
        var manifest = EntityIdManifestParser.Parse(xml, "manifest.xml", sink);

        Assert.NotNull(manifest);
        Assert.Empty(diags);
        Assert.Equal(2, manifest!.ActiveByName.Count);
        Assert.Equal(1, manifest.ActiveByName["Avatar"]);
        Assert.Equal(2, manifest.ActiveByName["Account"]);
        Assert.Contains((ushort)3, manifest.DeprecatedIds);
        Assert.Contains("_removed", manifest.DeprecatedNames);
    }

    [Fact]
    public void Parse_MalformedXml_EmitsDef025()
    {
        var diags = Capture(out var sink);
        var manifest = EntityIdManifestParser.Parse("<entity_ids><not closed", "x.xml", sink);

        Assert.Null(manifest);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF025");
    }

    [Fact]
    public void Parse_WrongRoot_EmitsDef025()
    {
        var diags = Capture(out var sink);
        var manifest = EntityIdManifestParser.Parse("<wrong/>", "x.xml", sink);

        Assert.Null(manifest);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF025");
    }

    [Fact]
    public void Parse_MissingId_EmitsDef019()
    {
        var xml = @"<entity_ids><entity name=""Avatar""/></entity_ids>";
        var diags = Capture(out var sink);
        var manifest = EntityIdManifestParser.Parse(xml, "x.xml", sink);

        Assert.NotNull(manifest);
        Assert.Empty(manifest!.ActiveByName);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF019");
    }

    [Theory]
    [InlineData("0")]
    [InlineData("16384")]
    [InlineData("-5")]
    public void Parse_IdOutOfRange_EmitsDef020(string idValue)
    {
        var xml = $@"<entity_ids><entity name=""Bad"" id=""{idValue}""/></entity_ids>";
        var diags = Capture(out var sink);
        EntityIdManifestParser.Parse(xml, "x.xml", sink);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF020");
    }

    [Fact]
    public void Parse_DuplicateId_EmitsDef021()
    {
        var xml = @"<entity_ids>
  <entity name=""A"" id=""5""/>
  <entity name=""B"" id=""5""/>
</entity_ids>";
        var diags = Capture(out var sink);
        EntityIdManifestParser.Parse(xml, "x.xml", sink);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF021");
    }

    [Fact]
    public void Parse_DuplicateName_EmitsDef026()
    {
        var xml = @"<entity_ids>
  <entity name=""A"" id=""1""/>
  <entity name=""A"" id=""2""/>
</entity_ids>";
        var diags = Capture(out var sink);
        EntityIdManifestParser.Parse(xml, "x.xml", sink);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF026");
    }

    [Fact]
    public void Parse_DeprecatedEntry_GoesToDeprecatedSets_NotActive()
    {
        var xml = @"<entity_ids>
  <entity name=""Live"" id=""1""/>
  <entity name=""Gone"" id=""2"" deprecated=""true""/>
</entity_ids>";
        var manifest = EntityIdManifestParser.Parse(xml, "x.xml", null);

        Assert.NotNull(manifest);
        Assert.True(manifest!.ActiveByName.ContainsKey("Live"));
        Assert.False(manifest.ActiveByName.ContainsKey("Gone"));
        Assert.Contains("Gone", manifest.DeprecatedNames);
        Assert.Contains((ushort)2, manifest.DeprecatedIds);
    }
}
