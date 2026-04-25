using Atlas.Components;
using Atlas.Entity;
using Xunit;

namespace Atlas.Tests;

[Entity("AvatarWithAbility")]
public partial class AvatarWithAbility : ServerEntity
{
}

[Entity("MonsterWithAbility")]
public partial class MonsterWithAbility : ServerEntity
{
}

// Cross-entity component reuse + inheritance:
//
//   AbilityComponent (standalone, base) — `cooldown` at propIdx 0
//      ↑ extends
//   AvatarAbility    (standalone, leaf) — `ranking` at propIdx 1
//   MonsterAbility   (standalone, leaf) — `aggro` at propIdx 1
//
// Both AvatarWithAbility and MonsterWithAbility reference these
// derived components by name without redeclaring properties.
public class ComponentReuseAndInheritanceTests
{
    [Fact]
    public void DerivedComponent_InheritsBaseProperty()
    {
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();
        Assert.Same(ability, avatar.Ability);

        // Cooldown comes from AbilityComponent (base); Ranking from
        // AvatarAbility (derived). Both are reachable through the
        // derived instance via C# inheritance.
        ability.Cooldown = 1.5f;
        ability.Ranking = 7;

        Assert.Equal(1.5f, ability.Cooldown);
        Assert.Equal(7, ability.Ranking);
        Assert.True(ability.IsDirty);
    }

    [Fact]
    public void TwoEntitiesShareSameBase_DifferentDerived()
    {
        // Avatar uses AvatarAbility (cooldown + ranking).
        // Monster uses MonsterAbility (cooldown + aggro).
        // Both derive from AbilityComponent so neither needs to
        // redeclare cooldown.
        var avatar = new AvatarWithAbility();
        var monster = new MonsterWithAbility();

        var ava = avatar.AddComponent<AvatarAbility>();
        var mon = monster.AddComponent<MonsterAbility>();

        ava.Cooldown = 2.0f;
        ava.Ranking = 99;

        mon.Cooldown = 0.5f;
        mon.Aggro = 1500;

        Assert.Equal(2.0f, ava.Cooldown);
        Assert.Equal(99, ava.Ranking);
        Assert.Equal(0.5f, mon.Cooldown);
        Assert.Equal(1500, mon.Aggro);
    }

    [Fact]
    public void DerivedClass_ExtendsBaseClassInCSharp()
    {
        // Verify C# inheritance is real — an AvatarAbility instance
        // should be assignable to AbilityComponent.
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();
        AbilityComponent baseRef = ability;
        Assert.NotNull(baseRef);
        Assert.IsType<AvatarAbility>(baseRef);
    }

    [Fact]
    public void BaseAndDerived_PropertyDirtyBitsAreDistinct()
    {
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();

        // Set the derived prop only (Ranking, propIdx 1). Base
        // Cooldown (propIdx 0) is untouched.
        ability.Cooldown = 1.0f;
        ability.ClearDirty();

        ability.Ranking = 5;
        Assert.True(ability.IsDirty);
    }
}
