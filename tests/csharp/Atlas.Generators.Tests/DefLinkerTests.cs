using System.Collections.Generic;
using System.Linq;
using Atlas.Generators.Def;
using Microsoft.CodeAnalysis;
using Xunit;

namespace Atlas.Generators.Tests;

// Targets the cross-entity pass: struct/alias uniqueness across the whole
// project, alias chain resolution, and the guards against pathological
// inputs that could otherwise hang or silently lose information.
public class DefLinkerTests
{
    private static EntityDefModel MakeEntity(string name,
                                             IEnumerable<StructDefModel>? structs = null,
                                             IEnumerable<AliasDefModel>? aliases = null)
    {
        var e = new EntityDefModel { Name = name };
        if (structs != null) foreach (var s in structs) e.Structs.Add(s);
        if (aliases != null) foreach (var a in aliases) e.Aliases.Add(a);
        return e;
    }

    private static StructDefModel Scalar(string name, string fieldName = "value",
                                         PropertyDataKind fieldKind = PropertyDataKind.Int32)
    {
        var s = new StructDefModel { Name = name };
        s.Fields.Add(new FieldDefModel
        {
            Name = fieldName,
            Type = new DataTypeRefModel { Kind = fieldKind },
        });
        return s;
    }

    [Fact]
    public void AliasChain_ResolvesThroughMultipleHops()
    {
        // alias A → alias B → alias C → struct ItemStack. All three aliases
        // are declared on a single entity alongside the struct they
        // ultimately reference.
        var itemStack = Scalar("ItemStack");
        var aliasC = new AliasDefModel
        {
            Name = "C",
            Target = new DataTypeRefModel { Kind = PropertyDataKind.Struct, StructName = "ItemStack" },
        };
        var aliasB = new AliasDefModel
        {
            Name = "B",
            Target = new DataTypeRefModel { Kind = PropertyDataKind.Struct, StructName = "C" },
        };
        var aliasA = new AliasDefModel
        {
            Name = "A",
            Target = new DataTypeRefModel { Kind = PropertyDataKind.Struct, StructName = "B" },
        };
        var entity = MakeEntity("Demo",
            structs: new[] { itemStack },
            aliases: new[] { aliasC, aliasB, aliasA });

        // Add a property whose TypeRef is A — this triggers the chain
        // resolution path in ResolveTypeRef.
        entity.Properties.Add(new PropertyDefModel
        {
            Name = "weapon", Type = "A", Scope = PropertyScope.AllClients,
            TypeRef = new DataTypeRefModel { Kind = PropertyDataKind.Struct, StructName = "A" },
        });

        var diags = new List<Diagnostic>();
        var linked = DefLinker.Link(new List<EntityDefModel> { entity }, diags.Add);
        Assert.NotNull(linked);
        Assert.Empty(diags);

        // After link: the property TypeRef has been collapsed all the way
        // down to the struct id of ItemStack (1 — only one struct).
        var pref = entity.Properties[0].TypeRef!;
        Assert.Equal(PropertyDataKind.Struct, pref.Kind);
        Assert.Equal("ItemStack", pref.StructName);
        Assert.Equal(1, pref.StructId);
    }

    [Fact]
    public void AliasCycle_RejectedWithoutStackOverflow()
    {
        // A → B → A. Without the depth cap the linker would recurse forever
        // and smash the stack; DEF017 should fire instead.
        var aliasA = new AliasDefModel
        {
            Name = "A",
            Target = new DataTypeRefModel { Kind = PropertyDataKind.Struct, StructName = "B" },
        };
        var aliasB = new AliasDefModel
        {
            Name = "B",
            Target = new DataTypeRefModel { Kind = PropertyDataKind.Struct, StructName = "A" },
        };
        var entity = MakeEntity("Demo", aliases: new[] { aliasA, aliasB });
        entity.Properties.Add(new PropertyDefModel
        {
            Name = "prop", Type = "A", Scope = PropertyScope.AllClients,
            TypeRef = new DataTypeRefModel { Kind = PropertyDataKind.Struct, StructName = "A" },
        });

        var diags = new List<Diagnostic>();
        var linked = DefLinker.Link(new List<EntityDefModel> { entity }, diags.Add);
        Assert.Null(linked);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF017");
    }

    [Fact]
    public void CrossEntityAliasStructCollision_Rejected()
    {
        // Entity X declares `alias Foo` first (in linker processing order —
        // alphabetical by name below), then entity Y declares `struct Foo`.
        // Without the guard the struct silently wins and the alias is
        // dropped; with DEF016 the link fails loud.
        var entityX = MakeEntity("Xentity",
            aliases: new[]
            {
                new AliasDefModel
                {
                    Name = "Foo",
                    Target = new DataTypeRefModel { Kind = PropertyDataKind.Int32 },
                },
            });
        var entityY = MakeEntity("Yentity", structs: new[] { Scalar("Foo") });

        var diags = new List<Diagnostic>();
        var linked = DefLinker.Link(new List<EntityDefModel> { entityX, entityY }, diags.Add);
        Assert.Null(linked);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF016");
    }

    [Fact]
    public void CrossEntityStructAliasCollision_Rejected()
    {
        // Reverse order: struct first, alias after — also rejected. Makes
        // the guard order-independent so the diagnostic doesn't depend on
        // which .def happens to be enumerated first.
        var entityX = MakeEntity("Xentity", structs: new[] { Scalar("Foo") });
        var entityY = MakeEntity("Yentity",
            aliases: new[]
            {
                new AliasDefModel
                {
                    Name = "Foo",
                    Target = new DataTypeRefModel { Kind = PropertyDataKind.Int32 },
                },
            });

        var diags = new List<Diagnostic>();
        var linked = DefLinker.Link(new List<EntityDefModel> { entityX, entityY }, diags.Add);
        Assert.Null(linked);
        Assert.Contains(diags, d => d.Id == "ATLAS_DEF016");
    }

    [Fact]
    public void LinkedDefs_StructsByNameContainsEveryLinkedStruct()
    {
        // The emitters consume linked.StructsByName; guarantee it covers
        // every struct DefLinker also placed in linked.Structs so a
        // divergence between the two maps can't creep in.
        var e1 = MakeEntity("E1", structs: new[] { Scalar("A"), Scalar("B") });
        var e2 = MakeEntity("E2", structs: new[] { Scalar("C") });

        var linked = DefLinker.Link(new List<EntityDefModel> { e1, e2 }, null);
        Assert.NotNull(linked);
        Assert.Equal(3, linked!.Structs.Count);
        Assert.Equal(3, linked.StructsByName.Count);
        foreach (var s in linked.Structs)
            Assert.Same(s, linked.StructsByName[s.Name]);
    }
}
