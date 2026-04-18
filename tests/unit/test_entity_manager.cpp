#include <gtest/gtest.h>

#include "entity_manager.h"
#include "id_client.h"

using namespace atlas;

// ============================================================================
// Helper: create an EntityManager with an IDClient pre-loaded with IDs
// ============================================================================

namespace {

struct TestFixture {
  IDClient client;
  EntityManager mgr;

  TestFixture() {
    client.AddIds(1, 2000);  // plenty of IDs above all watermarks
    mgr.SetIdClient(&client);
  }
};

}  // namespace

// ============================================================================
// EntityManager tests
// ============================================================================

TEST(EntityManager, AllocateIDsUnique) {
  TestFixture f;
  EntityID id1 = f.mgr.AllocateId();
  EntityID id2 = f.mgr.AllocateId();
  EntityID id3 = f.mgr.AllocateId();
  EXPECT_NE(id1, kInvalidEntityID);
  EXPECT_NE(id2, kInvalidEntityID);
  EXPECT_NE(id1, id2);
  EXPECT_NE(id2, id3);
}

TEST(EntityManager, CreateFailsWhenIDsExhausted) {
  IDClient client;
  EntityManager mgr;
  // No IDs added — IDClient is critically low
  mgr.SetIdClient(&client);

  auto* ent = mgr.Create(1, false);
  EXPECT_EQ(ent, nullptr);
  EXPECT_EQ(mgr.Size(), 0u);
}

TEST(EntityManager, CreateAndFind) {
  TestFixture f;
  auto* ent = f.mgr.Create(42, false);
  ASSERT_NE(ent, nullptr);
  EXPECT_EQ(ent->TypeId(), 42u);
  EXPECT_EQ(ent->Dbid(), kInvalidDBID);
  EXPECT_EQ(f.mgr.Size(), 1u);

  auto* found = f.mgr.Find(ent->EntityId());
  EXPECT_EQ(found, ent);
}

TEST(EntityManager, CreateProxy) {
  TestFixture f;
  auto* ent = f.mgr.Create(7, true);
  ASSERT_NE(ent, nullptr);
  EXPECT_EQ(f.mgr.Size(), 1u);

  auto* proxy = f.mgr.FindProxy(ent->EntityId());
  ASSERT_NE(proxy, nullptr);
  EXPECT_FALSE(proxy->HasClient());
}

TEST(EntityManager, FindByDbidUsesSecondaryIndex) {
  TestFixture f;
  auto* ent = f.mgr.Create(7, false);
  ASSERT_NE(ent, nullptr);

  EXPECT_EQ(f.mgr.FindByDbid(1001), nullptr);
  EXPECT_TRUE(f.mgr.AssignDbid(ent->EntityId(), 1001));
  EXPECT_EQ(f.mgr.FindByDbid(1001), ent);

  f.mgr.Destroy(ent->EntityId());
  EXPECT_EQ(f.mgr.FindByDbid(1001), nullptr);
}

TEST(EntityManager, FindProxyBySessionUsesSecondaryIndex) {
  TestFixture f;
  auto* ent = f.mgr.Create(9, true);
  ASSERT_NE(ent, nullptr);

  auto key = SessionKey::Generate();
  EXPECT_EQ(f.mgr.FindProxyBySession(key), nullptr);
  EXPECT_TRUE(f.mgr.AssignSessionKey(ent->EntityId(), key));
  EXPECT_EQ(f.mgr.FindProxyBySession(key), f.mgr.FindProxy(ent->EntityId()));

  EXPECT_TRUE(f.mgr.ClearSessionKey(ent->EntityId()));
  EXPECT_EQ(f.mgr.FindProxyBySession(key), nullptr);
}

TEST(EntityManager, DuplicateDbidAssignmentIsRejected) {
  TestFixture f;
  auto* a = f.mgr.Create(1, false);
  auto* b = f.mgr.Create(2, false);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  EXPECT_TRUE(f.mgr.AssignDbid(a->EntityId(), 77));
  EXPECT_FALSE(f.mgr.AssignDbid(b->EntityId(), 77));
  EXPECT_EQ(f.mgr.FindByDbid(77), a);
}

TEST(EntityManager, CreateRejectsDuplicateDbid) {
  TestFixture f;

  auto* first = f.mgr.Create(1, false, 1234);
  ASSERT_NE(first, nullptr);

  auto* second = f.mgr.Create(2, false, 1234);
  EXPECT_EQ(second, nullptr);
  EXPECT_EQ(f.mgr.FindByDbid(1234), first);
  EXPECT_EQ(f.mgr.Size(), 1u);
}

TEST(EntityManager, NonProxyFindProxy) {
  TestFixture f;
  auto* ent = f.mgr.Create(7, false);
  EXPECT_EQ(f.mgr.FindProxy(ent->EntityId()), nullptr);
}

TEST(EntityManager, Destroy) {
  TestFixture f;
  auto* ent = f.mgr.Create(1, false);
  EntityID id = ent->EntityId();
  f.mgr.Destroy(id);
  EXPECT_EQ(f.mgr.Find(id), nullptr);
  EXPECT_EQ(f.mgr.Size(), 0u);
}

TEST(EntityManager, FlushDestroyed) {
  TestFixture f;
  auto* ent1 = f.mgr.Create(1, false);
  auto* ent2 = f.mgr.Create(2, false);
  EntityID id1 = ent1->EntityId();

  ent1->MarkForDestroy();
  f.mgr.FlushDestroyed();

  EXPECT_EQ(f.mgr.Find(id1), nullptr);
  EXPECT_NE(f.mgr.Find(ent2->EntityId()), nullptr);
  EXPECT_EQ(f.mgr.Size(), 1u);
}

TEST(EntityManager, ForEach) {
  TestFixture f;
  f.mgr.Create(1, false);
  f.mgr.Create(2, false);
  f.mgr.Create(3, true);

  int count = 0;
  f.mgr.ForEach([&](const BaseEntity&) { ++count; });
  EXPECT_EQ(count, 3);
}

TEST(EntityManager, NoIDClientReturnsInvalid) {
  EntityManager mgr;
  // No IDClient installed
  EXPECT_EQ(mgr.AllocateId(), kInvalidEntityID);
  EXPECT_TRUE(mgr.IsRangeLow());
}
