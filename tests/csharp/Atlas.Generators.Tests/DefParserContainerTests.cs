using System.Collections.Generic;
using System.Linq;
using Atlas.Generators.Def;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;
using Xunit;

namespace Atlas.Generators.Tests;

// Covers the recursive type-expr parser, <types> section parsing, cyclic
// reference detection, max_size parsing, and the parser diagnostics for
// malformed container descriptors. These all live on the DefParser /
// DefTypeExprParser side — the emitter + native registration end is
// exercised separately.
public class DefParserContainerTests
{
    private static List<Diagnostic> CaptureDiagnostics()
    {
        return new List<Diagnostic>();
    }

    private static DataTypeRefModel? ParseExpr(string expr, out List<Diagnostic> diagnostics)
    {
        diagnostics = CaptureDiagnostics();
        return DefTypeExprParser.Parse(expr, diagnostics.Add);
    }

    // =========================================================================
    // DefTypeExprParser — scalar and container expressions
    // =========================================================================

    [Theory]
    // PropertyDataKind is internal to the generator project; passing the
    // numeric byte value keeps the [Theory] method signature public.
    [InlineData("int32", (byte)5)]    // Int32
    [InlineData("UInt64", (byte)8)]   // case-insensitive scalars
    [InlineData("  string ", (byte)11)]  // whitespace tolerated
    [InlineData("vector3", (byte)13)]
    [InlineData("quaternion", (byte)14)]
    [InlineData("bytes", (byte)12)]
    public void TypeExpr_ScalarsDecode(string expr, byte expectedKindByte)
    {
        var t = ParseExpr(expr, out var diags);
        Assert.NotNull(t);
        Assert.Empty(diags);
        Assert.Equal((PropertyDataKind)expectedKindByte, t!.Kind);
        Assert.Null(t.Elem);
        Assert.Null(t.Key);
    }

    [Fact]
    public void TypeExpr_ListOfInt32()
    {
        var t = ParseExpr("list[int32]", out var diags)!;
        Assert.Empty(diags);
        Assert.Equal(PropertyDataKind.List, t.Kind);
        Assert.NotNull(t.Elem);
        Assert.Equal(PropertyDataKind.Int32, t.Elem!.Kind);
    }

    [Fact]
    public void TypeExpr_DictStringToInt32()
    {
        var t = ParseExpr("dict[string,int32]", out var diags)!;
        Assert.Empty(diags);
        Assert.Equal(PropertyDataKind.Dict, t.Kind);
        Assert.NotNull(t.Key);
        Assert.NotNull(t.Elem);
        Assert.Equal(PropertyDataKind.String, t.Key!.Kind);
        Assert.Equal(PropertyDataKind.Int32, t.Elem!.Kind);
    }

    [Fact]
    public void TypeExpr_NestedListOfList()
    {
        var t = ParseExpr("list[list[int32]]", out var diags)!;
        Assert.Empty(diags);
        Assert.Equal(PropertyDataKind.List, t.Kind);
        Assert.Equal(PropertyDataKind.List, t.Elem!.Kind);
        Assert.Equal(PropertyDataKind.Int32, t.Elem.Elem!.Kind);
    }

    [Fact]
    public void TypeExpr_DictValueCanBeContainer()
    {
        var t = ParseExpr("dict[uint32,list[int32]]", out var diags)!;
        Assert.Empty(diags);
        Assert.Equal(PropertyDataKind.Dict, t.Kind);
        Assert.Equal(PropertyDataKind.UInt32, t.Key!.Kind);
        Assert.Equal(PropertyDataKind.List, t.Elem!.Kind);
        Assert.Equal(PropertyDataKind.Int32, t.Elem.Elem!.Kind);
    }

    [Fact]
    public void TypeExpr_UnknownNameTreatedAsStructReference()
    {
        // Struct / alias references are resolved by DefLinker; the parser just
        // records the name so the link step has something to look up.
        var t = ParseExpr("ItemStack", out var diags)!;
        Assert.Empty(diags);
        Assert.Equal(PropertyDataKind.Struct, t.Kind);
        Assert.Equal("ItemStack", t.StructName);
        Assert.Equal(-1, t.StructId);
    }

    [Fact]
    public void TypeExpr_MalformedDictMissingComma()
    {
        var t = ParseExpr("dict[string]", out var diags);
        Assert.Null(t);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF009");
    }

    [Fact]
    public void TypeExpr_StructKindIsNotValidDictKey()
    {
        // `Foo` could refer to a user struct. Accepting struct as a dict key
        // would pin us into key-hashing machinery we have no plans to build.
        var t = ParseExpr("dict[Foo,int32]", out var diags);
        Assert.Null(t);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF011");
    }

    [Fact]
    public void TypeExpr_DictKeyMustBeScalar_Rejects_Float()
    {
        // Floats / doubles are technically scalar but aren't stable hash keys
        // (NaN, -0.0); disallow them explicitly.
        var t = ParseExpr("dict[float,int32]", out var diags);
        Assert.Null(t);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF011");
    }

    [Fact]
    public void TypeExpr_DepthOverflowRejected()
    {
        // MaxDepth = 8 means 8 recursive descents are allowed; emit 9 to trip.
        var expr = string.Concat(Enumerable.Repeat("list[", 9)) + "int32"
                 + string.Concat(Enumerable.Repeat("]", 9));
        var t = ParseExpr(expr, out var diags);
        Assert.Null(t);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF010");
    }

    [Fact]
    public void TypeExpr_EmptyStringRejected()
    {
        var t = ParseExpr("   ", out var diags);
        Assert.Null(t);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF009");
    }

    // =========================================================================
    // DefParser — <types> section
    // =========================================================================

    [Fact]
    public void Types_SectionParsesStructWithScalarFields()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack"">
      <field name=""id"" type=""int32"" />
      <field name=""count"" type=""uint16"" />
    </struct>
  </types>
  <properties>
    <property name=""hp"" type=""int32"" scope=""all_clients"" />
  </properties>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add)!;

        Assert.Empty(diags);
        Assert.Single(model.Structs);
        var s = model.Structs[0];
        Assert.Equal("ItemStack", s.Name);
        Assert.Equal(2, s.Fields.Count);
        Assert.Equal(PropertyDataKind.Int32, s.Fields[0].Type.Kind);
        Assert.Equal(PropertyDataKind.UInt16, s.Fields[1].Type.Kind);
    }

    [Fact]
    public void Types_StructFieldCanBeContainer()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""Inventory"">
      <field name=""slots"" type=""list[int32]"" />
    </struct>
  </types>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add)!;

        Assert.Empty(diags);
        Assert.Single(model.Structs);
        var s = model.Structs[0];
        var slots = s.Fields[0].Type;
        Assert.Equal(PropertyDataKind.List, slots.Kind);
        Assert.Equal(PropertyDataKind.Int32, slots.Elem!.Kind);
    }

    [Fact]
    public void Types_StructFieldReferencesAnotherStruct()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
    <struct name=""Equipment""><field name=""weapon"" type=""ItemStack"" /></struct>
  </types>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add)!;

        Assert.Empty(diags);
        Assert.Equal(2, model.Structs.Count);
        var eq = model.Structs.Single(s => s.Name == "Equipment");
        Assert.Equal(PropertyDataKind.Struct, eq.Fields[0].Type.Kind);
        Assert.Equal("ItemStack", eq.Fields[0].Type.StructName);
    }

    [Fact]
    public void Types_AliasDeclarationAccepted()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
    <alias name=""Inventory"" type=""list[ItemStack]"" />
  </types>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add)!;

        Assert.Empty(diags);
        Assert.Single(model.Aliases);
        var alias = model.Aliases[0];
        Assert.Equal("Inventory", alias.Name);
        Assert.Equal(PropertyDataKind.List, alias.Target.Kind);
        Assert.Equal(PropertyDataKind.Struct, alias.Target.Elem!.Kind);
        Assert.Equal("ItemStack", alias.Target.Elem.StructName);
    }

    [Fact]
    public void Types_DuplicateStructNameRejected()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct>
    <struct name=""ItemStack""><field name=""other"" type=""float"" /></struct>
  </types>
</entity>";
        var diags = new List<Diagnostic>();
        DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF012");
    }

    [Fact]
    public void Types_CyclicStructReferenceRejected()
    {
        // Direct mutual recursion: A has field of type B, B has field of type A.
        // Without a cycle guard the emitter would blow out a stack building the
        // serializer, so block at parse time.
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""A""><field name=""b"" type=""B"" /></struct>
    <struct name=""B""><field name=""a"" type=""A"" /></struct>
  </types>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add);
        Assert.Null(model);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF013");
    }

    [Fact]
    public void Types_SelfReferenceRejected()
    {
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""Node""><field name=""next"" type=""Node"" /></struct>
  </types>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add);
        Assert.Null(model);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF013");
    }

    [Fact]
    public void Types_CycleViaContainerStillRejected()
    {
        // A field typed list[B] or dict[_,B] still creates a reference edge
        // A→B; the cycle detector must traverse container children.
        var xml = @"<entity name=""Avatar"">
  <types>
    <struct name=""A""><field name=""bs"" type=""list[B]"" /></struct>
    <struct name=""B""><field name=""a"" type=""A"" /></struct>
  </types>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add);
        Assert.Null(model);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF013");
    }

    // =========================================================================
    // DefParser — property type_ref & max_size
    // =========================================================================

    [Fact]
    public void Property_ContainerPopulatesTypeRef()
    {
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""bag"" type=""list[int32]"" scope=""own_client"" max_size=""128"" />
  </properties>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add)!;

        Assert.Empty(diags);
        var p = model.Properties[0];
        Assert.NotNull(p.TypeRef);
        Assert.Equal(PropertyDataKind.List, p.TypeRef!.Kind);
        Assert.Equal(PropertyDataKind.Int32, p.TypeRef.Elem!.Kind);
        Assert.Equal(128u, p.MaxSize);
    }

    [Fact]
    public void Property_ScalarLeavesTypeRefNull()
    {
        // Only containers get a TypeRef tail; scalar properties keep the
        // legacy flat `Type` string and the existing emitter path.
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp"" type=""int32"" scope=""all_clients"" />
  </properties>
</entity>";
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", null)!;
        var p = model.Properties[0];
        Assert.Null(p.TypeRef);
        Assert.Equal(4096u, p.MaxSize);  // default
    }

    [Fact]
    public void Property_ContainerWithoutMaxSizeKeepsDefault()
    {
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""bag"" type=""list[int32]"" scope=""own_client"" />
  </properties>
</entity>";
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", null)!;
        Assert.Equal(4096u, model.Properties[0].MaxSize);
    }

    [Fact]
    public void Property_StructTypedPropertyRecordsStructReference()
    {
        var xml = @"<entity name=""Avatar"">
  <types><struct name=""ItemStack""><field name=""id"" type=""int32"" /></struct></types>
  <properties>
    <property name=""weapon"" type=""ItemStack"" scope=""all_clients"" />
  </properties>
</entity>";
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", null)!;
        var p = model.Properties[0];
        Assert.NotNull(p.TypeRef);
        Assert.Equal(PropertyDataKind.Struct, p.TypeRef!.Kind);
        Assert.Equal("ItemStack", p.TypeRef.StructName);
    }

    [Fact]
    public void Property_MalformedContainerTypeFailsParse()
    {
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""bag"" type=""list[]"" scope=""own_client"" />
  </properties>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add);
        Assert.Null(model);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF009");
    }

    // =========================================================================
    // Regression guard — existing scalar-only defs must keep parsing
    // =========================================================================

    [Fact]
    public void DuplicatePropertyName_Rejected()
    {
        // Two <property name="hp"> slipped past the old parser and crashed
        // the generator later (duplicate C# property names — cryptic
        // compile error). DEF015 fails the parse upfront.
        var xml = @"<entity name=""Avatar"">
  <properties>
    <property name=""hp"" type=""int32"" scope=""all_clients"" />
    <property name=""hp"" type=""int32"" scope=""all_clients"" />
  </properties>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Avatar.def", diags.Add);
        Assert.Null(model);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF015");
    }

    [Fact]
    public void Existing_ScalarOnlyEntityDefStillParses()
    {
        // Guards against the types-section / typeref / max_size additions
        // accidentally breaking scalar-only entity defs — i.e. defs that
        // don't reference any of the container or struct features.
        var xml = @"<entity name=""Account"">
  <properties>
    <property name=""username"" type=""string"" scope=""base"" persistent=""true"" />
    <property name=""loginKey"" type=""string"" scope=""base_and_client"" persistent=""true"" />
  </properties>
  <base_methods>
    <method name=""RequestAvatarList"" exposed=""own_client"" />
  </base_methods>
</entity>";
        var diags = new List<Diagnostic>();
        var model = DefParser.Parse(SourceText.From(xml), "Account.def", diags.Add);
        Assert.NotNull(model);
        Assert.Empty(diags);
        Assert.Equal("Account", model!.Name);
        Assert.Equal(2, model.Properties.Count);
        Assert.Null(model.Properties[0].TypeRef);
        Assert.Single(model.BaseMethods);
    }
}
