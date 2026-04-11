#include "entity_manager.hpp"

#include <gtest/gtest.h>

using namespace atlas;

// ============================================================================
// EntityManager tests
// ============================================================================

TEST(EntityManager, AllocateIDsUnique)
{
    EntityManager mgr(0);
    EntityID id1 = mgr.allocate_id();
    EntityID id2 = mgr.allocate_id();
    EntityID id3 = mgr.allocate_id();
    EXPECT_NE(id1, kInvalidEntityID);
    EXPECT_NE(id2, kInvalidEntityID);
    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
}

TEST(EntityManager, AppIndexBuckets)
{
    EntityManager mgr0(0);
    EntityManager mgr1(1);
    EntityID a = mgr0.allocate_id();
    EntityID b = mgr1.allocate_id();
    // IDs from different app indices must not overlap in the first N allocations
    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, EntityManager::kIdBucketSize + 1u);
}

TEST(EntityManager, AllocateIDStopsAtRangeEnd)
{
    EntityManager mgr;
    mgr.set_id_range(10, 12);

    EXPECT_EQ(mgr.allocate_id(), 10u);
    EXPECT_EQ(mgr.allocate_id(), 11u);
    EXPECT_EQ(mgr.allocate_id(), 12u);
    EXPECT_EQ(mgr.allocate_id(), kInvalidEntityID);
    EXPECT_EQ(mgr.range_remaining(), 0u);
}

TEST(EntityManager, CreateFailsWhenRangeExhausted)
{
    EntityManager mgr;
    mgr.set_id_range(50, 50);

    auto* first = mgr.create(1, false);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->entity_id(), 50u);

    auto* second = mgr.create(2, false);
    EXPECT_EQ(second, nullptr);
    EXPECT_EQ(mgr.size(), 1u);
}

TEST(EntityManager, RangeLowUsesAssignedRangeCapacity)
{
    EntityManager mgr;
    mgr.set_id_range(100, 109);

    EXPECT_FALSE(mgr.is_range_low());

    for (int i = 0; i < 9; ++i)
    {
        ASSERT_NE(mgr.allocate_id(), kInvalidEntityID);
    }

    EXPECT_TRUE(mgr.is_range_low());
}

TEST(EntityManager, CreateAndFind)
{
    EntityManager mgr;
    auto* ent = mgr.create(42, false);
    ASSERT_NE(ent, nullptr);
    EXPECT_EQ(ent->type_id(), 42u);
    EXPECT_EQ(ent->dbid(), kInvalidDBID);
    EXPECT_EQ(mgr.size(), 1u);

    auto* found = mgr.find(ent->entity_id());
    EXPECT_EQ(found, ent);
}

TEST(EntityManager, CreateProxy)
{
    EntityManager mgr;
    auto* ent = mgr.create(7, true);
    ASSERT_NE(ent, nullptr);
    EXPECT_EQ(mgr.size(), 1u);

    auto* proxy = mgr.find_proxy(ent->entity_id());
    ASSERT_NE(proxy, nullptr);
    EXPECT_FALSE(proxy->has_client());
}

TEST(EntityManager, FindByDbidUsesSecondaryIndex)
{
    EntityManager mgr;
    auto* ent = mgr.create(7, false);
    ASSERT_NE(ent, nullptr);

    EXPECT_EQ(mgr.find_by_dbid(1001), nullptr);
    EXPECT_TRUE(mgr.assign_dbid(ent->entity_id(), 1001));
    EXPECT_EQ(mgr.find_by_dbid(1001), ent);

    mgr.destroy(ent->entity_id());
    EXPECT_EQ(mgr.find_by_dbid(1001), nullptr);
}

TEST(EntityManager, FindProxyBySessionUsesSecondaryIndex)
{
    EntityManager mgr;
    auto* ent = mgr.create(9, true);
    ASSERT_NE(ent, nullptr);

    auto key = SessionKey::generate();
    EXPECT_EQ(mgr.find_proxy_by_session(key), nullptr);
    EXPECT_TRUE(mgr.assign_session_key(ent->entity_id(), key));
    EXPECT_EQ(mgr.find_proxy_by_session(key), mgr.find_proxy(ent->entity_id()));

    EXPECT_TRUE(mgr.clear_session_key(ent->entity_id()));
    EXPECT_EQ(mgr.find_proxy_by_session(key), nullptr);
}

TEST(EntityManager, DuplicateDbidAssignmentIsRejected)
{
    EntityManager mgr;
    auto* a = mgr.create(1, false);
    auto* b = mgr.create(2, false);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_TRUE(mgr.assign_dbid(a->entity_id(), 77));
    EXPECT_FALSE(mgr.assign_dbid(b->entity_id(), 77));
    EXPECT_EQ(mgr.find_by_dbid(77), a);
}

TEST(EntityManager, NonProxyFindProxy)
{
    EntityManager mgr;
    auto* ent = mgr.create(7, false);
    EXPECT_EQ(mgr.find_proxy(ent->entity_id()), nullptr);
}

TEST(EntityManager, Destroy)
{
    EntityManager mgr;
    auto* ent = mgr.create(1, false);
    EntityID id = ent->entity_id();
    mgr.destroy(id);
    EXPECT_EQ(mgr.find(id), nullptr);
    EXPECT_EQ(mgr.size(), 0u);
}

TEST(EntityManager, FlushDestroyed)
{
    EntityManager mgr;
    auto* ent1 = mgr.create(1, false);
    auto* ent2 = mgr.create(2, false);
    EntityID id1 = ent1->entity_id();

    ent1->mark_for_destroy();
    mgr.flush_destroyed();

    EXPECT_EQ(mgr.find(id1), nullptr);
    EXPECT_NE(mgr.find(ent2->entity_id()), nullptr);
    EXPECT_EQ(mgr.size(), 1u);
}

TEST(EntityManager, ForEach)
{
    EntityManager mgr;
    mgr.create(1, false);
    mgr.create(2, false);
    mgr.create(3, true);

    int count = 0;
    mgr.for_each([&](const BaseEntity&) { ++count; });
    EXPECT_EQ(count, 3);
}
