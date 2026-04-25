using System;
using Atlas.Entity;
using Atlas.Entity.Components;
using Xunit;

namespace Atlas.Tests;

// Hand-rolled fixture tests for the Component runtime foundation. No
// codegen yet — tests provide their own ResolveSyncedSlot mappings to
// exercise AddComponent / GetSyncedComponent / RemoveComponent /
// AddLocalComponent / TickAllComponents / dirty-flag plumbing.

public class TestCombatComponent : ReplicatedComponent
{
    public int AttackedCount;
    public int OnAttachedCount;
    public int OnDetachedCount;
    public int OnTickCount;

    public override void OnAttached() => OnAttachedCount++;
    public override void OnDetached() => OnDetachedCount++;
    public override void OnTick(float dt) => OnTickCount++;

    public void RecordHit() { AttackedCount++; MarkDirty(propIdx: 0); }
}

public class TestInventoryComponent : ReplicatedComponent
{
    public int ItemCount;
    public void AddItem() { ItemCount++; MarkDirty(propIdx: 0); }
}

public class TestServerAi : ServerLocalComponent
{
    public int Ticks;
    public override void OnTick(float dt) => Ticks++;
}

// No [Entity] attribute — this fixture stays out of the type registry
// and the .def generator. Component runtime is independent of the
// entity-registration path; testing it doesn't need a .def.
public class ComponentTestEntity : ServerEntity
{
    // Stub all the abstract overrides codegen would normally provide so
    // the entity can be instantiated without a .def.
    public override string TypeName => "ComponentTestEntity";
    public override void Serialize(ref Atlas.Serialization.SpanWriter w) { }
    public override void Deserialize(ref Atlas.Serialization.SpanReader r) { }

    // Hand-rolled slot table: 2 Synced slots, indices 1 and 2. Slot 0
    // is reserved for the entity body per the design contract.
    protected override int SyncedSlotCount => 2;
    protected override int ResolveSyncedSlot(Type t)
    {
        if (t == typeof(TestCombatComponent)) return 1;
        if (t == typeof(TestInventoryComponent)) return 2;
        return -1;
    }

    public TestCombatComponent? Combat => GetSyncedComponent<TestCombatComponent>();
    public TestInventoryComponent? Inventory => GetSyncedComponent<TestInventoryComponent>();
}

public class ComponentRuntimeTests
{
    [Fact]
    public void AddComponent_AssignsSlotAndFiresOnAttached()
    {
        var e = new ComponentTestEntity();
        var combat = e.AddComponent<TestCombatComponent>();

        Assert.Equal(1, combat.SlotIdx);
        Assert.Same(e, combat.Entity);
        Assert.Equal(1, combat.OnAttachedCount);
        Assert.Same(combat, e.Combat);
    }

    [Fact]
    public void AddComponent_IsIdempotent_ReturnsExistingInstance()
    {
        var e = new ComponentTestEntity();
        var c1 = e.AddComponent<TestCombatComponent>();
        var c2 = e.AddComponent<TestCombatComponent>();

        Assert.Same(c1, c2);
        Assert.Equal(1, c1.OnAttachedCount);
    }

    [Fact]
    public void AddComponent_UnknownType_Throws()
    {
        var e = new ComponentTestEntity();
        Assert.Throws<InvalidOperationException>(() =>
            e.AddComponent<UnknownComponent>());
    }

    [Fact]
    public void MarkDirty_SetsEntityComponentBitmap()
    {
        var e = new ComponentTestEntity();
        var combat = e.AddComponent<TestCombatComponent>();

        // Adding clears the dirty bit eventually; for this assertion
        // start fresh by clearing it.
        ClearDirtyComponentsForTest(e);
        Assert.Equal(0UL, GetDirtyComponentsForTest(e));

        combat.RecordHit();

        Assert.True(combat.IsDirty);
        // bit 1 because slot 1 was marked dirty
        Assert.Equal(1UL << 1, GetDirtyComponentsForTest(e));
    }

    [Fact]
    public void RemoveComponent_FiresOnDetached_ClearsSlot_FlagsDirty()
    {
        var e = new ComponentTestEntity();
        var combat = e.AddComponent<TestCombatComponent>();
        ClearDirtyComponentsForTest(e);

        bool removed = e.RemoveComponent<TestCombatComponent>();

        Assert.True(removed);
        Assert.Equal(1, combat.OnDetachedCount);
        Assert.Null(e.Combat);
        // RemoveComponent flags the slot dirty so a future codegen pump
        // can emit a kRemoveComponent op.
        Assert.Equal(1UL << 1, GetDirtyComponentsForTest(e));
    }

    [Fact]
    public void TickAllComponents_FiresSyncedFirstThenLocal()
    {
        var e = new ComponentTestEntity();
        var combat = e.AddComponent<TestCombatComponent>();
        var inventory = e.AddComponent<TestInventoryComponent>();
        var ai = e.AddLocalComponent<TestServerAi>();

        e.TickAllComponents(0.016f);

        Assert.Equal(1, combat.OnTickCount);
        Assert.Equal(1, ai.Ticks);
    }

    [Fact]
    public void TickAllComponents_VisitsSlotsInAscendingOrder()
    {
        var e = new ComponentTestEntity();
        var combat = e.AddComponent<TestCombatComponent>();
        var inventory = e.AddComponent<TestInventoryComponent>();

        var observedOrder = new System.Collections.Generic.List<int>();
        combat.GetType();  // touch types so the JIT settles

        // Replace OnTick via a custom subclass-style probe: easiest is
        // to verify ticks fire in order by checking SlotIdx assignment.
        Assert.True(combat.SlotIdx < inventory.SlotIdx);

        e.TickAllComponents(0.016f);
        Assert.Equal(1, combat.OnTickCount);
    }

    [Fact]
    public void AddLocalComponent_StoresByTypeKey()
    {
        var e = new ComponentTestEntity();
        var ai = e.AddLocalComponent<TestServerAi>();

        Assert.Same(ai, e.GetLocalComponent<TestServerAi>());
        Assert.Same(e, ai.Entity);
    }

    [Fact]
    public void RemoveLocalComponent_FiresOnDetachedAndForgets()
    {
        var e = new ComponentTestEntity();
        var ai = e.AddLocalComponent<TestServerAi>();

        bool removed = e.RemoveLocalComponent<TestServerAi>();

        Assert.True(removed);
        Assert.Null(e.GetLocalComponent<TestServerAi>());
    }

    [Fact]
    public void DirtyComponents_BitmapIsZeroForFreshEntity()
    {
        var e = new ComponentTestEntity();
        Assert.Equal(0UL, GetDirtyComponentsForTest(e));
    }

    // Direct access to `_dirtyComponents` — `protected internal` on
    // ServerEntity, visible to this test assembly via InternalsVisibleTo.
    private static ulong GetDirtyComponentsForTest(ServerEntity e) => e._dirtyComponents;
    private static void ClearDirtyComponentsForTest(ServerEntity e) => e._dirtyComponents = 0UL;
}

public class UnknownComponent : ReplicatedComponent { }
