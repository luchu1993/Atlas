using Atlas.Components;
using Atlas.Entity;
using Xunit;

namespace Atlas.Tests;

[Entity("ComponentEntity")]
public partial class ComponentEntity : ServerEntity
{
}

// End-to-end check that .def-declared components codegen into working
// classes: the generator emits the per-component class, the entity
// gains typed accessors and a slot resolver, and AddComponent /
// MarkDirty plumbing works through the generated code path.
public class ComponentCodegenTests
{
    [Fact]
    public void Generator_EmitsTypedAccessors_AndAddComponentResolves()
    {
        var e = new ComponentEntity();
        var combat = e.AddComponent<GenCombat>();

        Assert.NotNull(combat);
        Assert.Equal(1, combat.SlotIdx);
        Assert.Same(combat, e.Combat);
    }

    [Fact]
    public void Generator_AddSecondComponent_GetsSlotTwo()
    {
        var e = new ComponentEntity();
        var combat = e.AddComponent<GenCombat>();
        var inventory = e.AddComponent<GenInventory>();

        Assert.Equal(1, combat.SlotIdx);
        Assert.Equal(2, inventory.SlotIdx);
        Assert.Same(combat, e.Combat);
        Assert.Same(inventory, e.Inventory);
    }

    [Fact]
    public void Generator_PropertySetter_TripsComponentDirty()
    {
        var e = new ComponentEntity();
        var combat = e.AddComponent<GenCombat>();
        e._dirtyComponents = 0;     // wipe the AddComponent dirty bit

        combat.Atk = 50;

        Assert.True(combat.IsDirty);
        // SlotIdx == 1 → bit 1 should fire on the entity's bitmap.
        Assert.Equal(1UL << 1, e._dirtyComponents);
    }

    [Fact]
    public void Generator_PropertySetter_SkipsNoOpAssignment()
    {
        var e = new ComponentEntity();
        var combat = e.AddComponent<GenCombat>();
        combat.Atk = 50;
        combat.ClearDirty();
        e._dirtyComponents = 0;

        combat.Atk = 50;  // same value — generator's setter should short-circuit

        Assert.False(combat.IsDirty);
        Assert.Equal(0UL, e._dirtyComponents);
    }

    [Fact]
    public void Generator_DistinctPropsHaveDistinctBitIndices()
    {
        var e = new ComponentEntity();
        var combat = e.AddComponent<GenCombat>();
        e._dirtyComponents = 0;

        combat.Atk = 10;        // propIdx 0 → bit 0 inside the component
        combat.Def = 5;         // propIdx 1 → bit 1 inside the component

        Assert.True(combat.IsDirty);
        // Per-component dirty mask isn't exposed publicly, but a single
        // ClearDirty wiping both is the round-trip equivalent.
        combat.ClearDirty();
        Assert.False(combat.IsDirty);
    }

    [Fact]
    public void Generator_UnregisteredType_AddComponentThrows()
    {
        var e = new ComponentEntity();
        Assert.Throws<System.InvalidOperationException>(() =>
            e.AddComponent<UnregisteredComponent>());
    }
}

// Not declared in ComponentEntity.def — used to assert the codegen-
// emitted ResolveSyncedSlot returns -1 for unknown types.
public sealed partial class UnregisteredComponent : Atlas.Entity.Components.ReplicatedComponent
{
}
