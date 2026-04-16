#include <gtest/gtest.h>

#include "base_entity.h"

using namespace atlas;

// ============================================================================
// BaseEntity tests
// ============================================================================

TEST(BaseEntity, DefaultState) {
  BaseEntity ent(42, 7);
  EXPECT_EQ(ent.entity_id(), 42u);
  EXPECT_EQ(ent.type_id(), 7u);
  EXPECT_EQ(ent.dbid(), kInvalidDBID);
  EXPECT_FALSE(ent.has_cell());
  EXPECT_FALSE(ent.is_pending_destroy());
  EXPECT_TRUE(ent.entity_data().empty());
}

TEST(BaseEntity, SetCellAndClear) {
  BaseEntity ent(1, 2);
  Address addr(0x7F000001u, 7001);
  ent.SetCell(99, addr);
  EXPECT_TRUE(ent.has_cell());
  EXPECT_EQ(ent.cell_entity_id(), 99u);
  EXPECT_EQ(ent.cell_addr().Port(), 7001u);

  ent.ClearCell();
  EXPECT_FALSE(ent.has_cell());
}

TEST(BaseEntity, EntityData) {
  BaseEntity ent(1, 2);
  std::vector<std::byte> data{std::byte{1}, std::byte{2}, std::byte{3}};
  ent.set_entity_data(data);
  EXPECT_EQ(ent.entity_data().size(), 3u);
  EXPECT_EQ(ent.entity_data()[0], std::byte{1});
}

TEST(BaseEntity, WriteAckSuccess) {
  BaseEntity ent(1, 2);
  ent.OnWriteAck(42, true);
  EXPECT_EQ(ent.dbid(), 42);
}

TEST(BaseEntity, WriteAckFailure) {
  BaseEntity ent(1, 2, 55);
  ent.OnWriteAck(0, false);
  // dbid should remain unchanged on failure
  EXPECT_EQ(ent.dbid(), 55);
}

TEST(BaseEntity, MarkForDestroy) {
  BaseEntity ent(1, 2);
  EXPECT_FALSE(ent.is_pending_destroy());
  ent.MarkForDestroy();
  EXPECT_TRUE(ent.is_pending_destroy());
}

// ============================================================================
// Proxy tests
// ============================================================================

TEST(Proxy, DefaultState) {
  Proxy p(10, 5);
  EXPECT_EQ(p.entity_id(), 10u);
  EXPECT_FALSE(p.has_client());
}

TEST(Proxy, SessionKey) {
  Proxy p(10, 5);
  SessionKey key{};
  key.bytes[0] = 0xAB;
  key.bytes[31] = 0xCD;
  p.set_session_key(key);
  EXPECT_EQ(p.session_key().bytes[0], 0xAB);
  EXPECT_EQ(p.session_key().bytes[31], 0xCD);
}
