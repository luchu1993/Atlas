#include "entity_manager.hpp"
#include "id_client.hpp"

#include <gtest/gtest.h>

using namespace atlas;

// ============================================================================
// Helper: create an EntityManager with an IDClient pre-loaded with IDs
// ============================================================================

namespace
{

struct TestFixture
{
    IDClient client;
    EntityManager mgr;

    TestFixture()
    {
        client.add_ids(1, 2000);  // plenty of IDs above all watermarks
        mgr.set_id_client(&client);
    }
};

}  // namespace

// ============================================================================
// EntityManager tests
// ============================================================================

TEST(EntityManager, AllocateIDsUnique)
{
    TestFixture f;
    EntityID id1 = f.mgr.allocate_id();
    EntityID id2 = f.mgr.allocate_id();
    EntityID id3 = f.mgr.allocate_id();
    EXPECT_NE(id1, kInvalidEntityID);
    EXPECT_NE(id2, kInvalidEntityID);
    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
}

TEST(EntityManager, CreateFailsWhenIDsExhausted)
{
    IDClient client;
    EntityManager mgr;
    // No IDs added — IDClient is critically low
    mgr.set_id_client(&client);

    auto* ent = mgr.create(1, false);
    EXPECT_EQ(ent, nullptr);
    EXPECT_EQ(mgr.size(), 0u);
}

TEST(EntityManager, CreateAndFind)
{
    TestFixture f;
    auto* ent = f.mgr.create(42, false);
    ASSERT_NE(ent, nullptr);
    EXPECT_EQ(ent->type_id(), 42u);
    EXPECT_EQ(ent->dbid(), kInvalidDBID);
    EXPECT_EQ(f.mgr.size(), 1u);

    auto* found = f.mgr.find(ent->entity_id());
    EXPECT_EQ(found, ent);
}

TEST(EntityManager, CreateProxy)
{
    TestFixture f;
    auto* ent = f.mgr.create(7, true);
    ASSERT_NE(ent, nullptr);
    EXPECT_EQ(f.mgr.size(), 1u);

    auto* proxy = f.mgr.find_proxy(ent->entity_id());
    ASSERT_NE(proxy, nullptr);
    EXPECT_FALSE(proxy->has_client());
}

TEST(EntityManager, FindByDbidUsesSecondaryIndex)
{
    TestFixture f;
    auto* ent = f.mgr.create(7, false);
    ASSERT_NE(ent, nullptr);

    EXPECT_EQ(f.mgr.find_by_dbid(1001), nullptr);
    EXPECT_TRUE(f.mgr.assign_dbid(ent->entity_id(), 1001));
    EXPECT_EQ(f.mgr.find_by_dbid(1001), ent);

    f.mgr.destroy(ent->entity_id());
    EXPECT_EQ(f.mgr.find_by_dbid(1001), nullptr);
}

TEST(EntityManager, FindProxyBySessionUsesSecondaryIndex)
{
    TestFixture f;
    auto* ent = f.mgr.create(9, true);
    ASSERT_NE(ent, nullptr);

    auto key = SessionKey::generate();
    EXPECT_EQ(f.mgr.find_proxy_by_session(key), nullptr);
    EXPECT_TRUE(f.mgr.assign_session_key(ent->entity_id(), key));
    EXPECT_EQ(f.mgr.find_proxy_by_session(key), f.mgr.find_proxy(ent->entity_id()));

    EXPECT_TRUE(f.mgr.clear_session_key(ent->entity_id()));
    EXPECT_EQ(f.mgr.find_proxy_by_session(key), nullptr);
}

TEST(EntityManager, DuplicateDbidAssignmentIsRejected)
{
    TestFixture f;
    auto* a = f.mgr.create(1, false);
    auto* b = f.mgr.create(2, false);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_TRUE(f.mgr.assign_dbid(a->entity_id(), 77));
    EXPECT_FALSE(f.mgr.assign_dbid(b->entity_id(), 77));
    EXPECT_EQ(f.mgr.find_by_dbid(77), a);
}

TEST(EntityManager, CreateRejectsDuplicateDbid)
{
    TestFixture f;

    auto* first = f.mgr.create(1, false, 1234);
    ASSERT_NE(first, nullptr);

    auto* second = f.mgr.create(2, false, 1234);
    EXPECT_EQ(second, nullptr);
    EXPECT_EQ(f.mgr.find_by_dbid(1234), first);
    EXPECT_EQ(f.mgr.size(), 1u);
}

TEST(EntityManager, NonProxyFindProxy)
{
    TestFixture f;
    auto* ent = f.mgr.create(7, false);
    EXPECT_EQ(f.mgr.find_proxy(ent->entity_id()), nullptr);
}

TEST(EntityManager, Destroy)
{
    TestFixture f;
    auto* ent = f.mgr.create(1, false);
    EntityID id = ent->entity_id();
    f.mgr.destroy(id);
    EXPECT_EQ(f.mgr.find(id), nullptr);
    EXPECT_EQ(f.mgr.size(), 0u);
}

TEST(EntityManager, FlushDestroyed)
{
    TestFixture f;
    auto* ent1 = f.mgr.create(1, false);
    auto* ent2 = f.mgr.create(2, false);
    EntityID id1 = ent1->entity_id();

    ent1->mark_for_destroy();
    f.mgr.flush_destroyed();

    EXPECT_EQ(f.mgr.find(id1), nullptr);
    EXPECT_NE(f.mgr.find(ent2->entity_id()), nullptr);
    EXPECT_EQ(f.mgr.size(), 1u);
}

TEST(EntityManager, ForEach)
{
    TestFixture f;
    f.mgr.create(1, false);
    f.mgr.create(2, false);
    f.mgr.create(3, true);

    int count = 0;
    f.mgr.for_each([&](const BaseEntity&) { ++count; });
    EXPECT_EQ(count, 3);
}

TEST(EntityManager, NoIDClientReturnsInvalid)
{
    EntityManager mgr;
    // No IDClient installed
    EXPECT_EQ(mgr.allocate_id(), kInvalidEntityID);
    EXPECT_TRUE(mgr.is_range_low());
}
