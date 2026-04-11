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
