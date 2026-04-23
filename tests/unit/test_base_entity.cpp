#include <gtest/gtest.h>

#include "base_entity.h"

using namespace atlas;

// ============================================================================
// BaseEntity tests
// ============================================================================

TEST(BaseEntity, DefaultState) {
  BaseEntity ent(42, 7);
  EXPECT_EQ(ent.EntityId(), 42u);
  EXPECT_EQ(ent.TypeId(), 7u);
  EXPECT_EQ(ent.Dbid(), kInvalidDBID);
  EXPECT_FALSE(ent.HasCell());
  EXPECT_FALSE(ent.IsPendingDestroy());
  EXPECT_TRUE(ent.EntityData().empty());
}

TEST(BaseEntity, SetCellAndClear) {
  BaseEntity ent(1, 2);
  Address addr(0x7F000001u, 7001);
  ent.SetCell(99, addr);
  EXPECT_TRUE(ent.HasCell());
  EXPECT_EQ(ent.CellEntityId(), 99u);
  EXPECT_EQ(ent.CellAddr().Port(), 7001u);

  ent.ClearCell();
  EXPECT_FALSE(ent.HasCell());
}

TEST(BaseEntity, EntityData) {
  BaseEntity ent(1, 2);
  std::vector<std::byte> data{std::byte{1}, std::byte{2}, std::byte{3}};
  ent.SetEntityData(data);
  EXPECT_EQ(ent.EntityData().size(), 3u);
  EXPECT_EQ(ent.EntityData()[0], std::byte{1});
}

TEST(BaseEntity, WriteAckSuccess) {
  BaseEntity ent(1, 2);
  ent.OnWriteAck(42, true);
  EXPECT_EQ(ent.Dbid(), 42);
}

TEST(BaseEntity, WriteAckFailure) {
  BaseEntity ent(1, 2, 55);
  ent.OnWriteAck(0, false);
  // dbid should remain unchanged on failure
  EXPECT_EQ(ent.Dbid(), 55);
}

TEST(BaseEntity, MarkForDestroy) {
  BaseEntity ent(1, 2);
  EXPECT_FALSE(ent.IsPendingDestroy());
  ent.MarkForDestroy();
  EXPECT_TRUE(ent.IsPendingDestroy());
}

// ============================================================================
// Proxy tests
// ============================================================================

TEST(Proxy, DefaultState) {
  Proxy p(10, 5);
  EXPECT_EQ(p.EntityId(), 10u);
  EXPECT_FALSE(p.HasClient());
}

TEST(Proxy, SessionKey) {
  Proxy p(10, 5);
  SessionKey key{};
  key.bytes[0] = 0xAB;
  key.bytes[31] = 0xCD;
  p.SetSessionKey(key);
  EXPECT_EQ(p.GetSessionKey().bytes[0], 0xAB);
  EXPECT_EQ(p.GetSessionKey().bytes[31], 0xCD);
}

// BaseEntity tracks the Space its cell counterpart lives in; needed by
// OnCellAppDeath to look up the correct rehome target.
TEST(BaseEntity, SpaceIdDefaultAndSet) {
  BaseEntity ent(1, 2);
  EXPECT_EQ(ent.SpaceId(), kInvalidSpaceID);
  ent.SetSpaceId(SpaceID{42});
  EXPECT_EQ(ent.SpaceId(), SpaceID{42});
}
