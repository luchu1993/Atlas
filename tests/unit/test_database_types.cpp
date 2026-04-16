#include <gtest/gtest.h>

#include "db/database_factory.h"
#include "db/idatabase.h"

using namespace atlas;

// ============================================================================
// WriteFlags helpers
// ============================================================================

TEST(DatabaseTypes, WriteFlagsHasFlag) {
  auto flags = static_cast<WriteFlags>(static_cast<uint8_t>(WriteFlags::kCreateNew) |
                                       static_cast<uint8_t>(WriteFlags::kAutoLoadOn));

  EXPECT_TRUE(HasFlag(flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(flags, WriteFlags::kAutoLoadOn));
  EXPECT_FALSE(HasFlag(flags, WriteFlags::kLogOff));
  EXPECT_FALSE(HasFlag(flags, WriteFlags::kDelete));
}

TEST(DatabaseTypes, WriteFlagsAndOp) {
  auto a = WriteFlags::kCreateNew;
  auto b = WriteFlags::kLogOff;
  auto combined = static_cast<WriteFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));

  EXPECT_TRUE(HasFlag(combined & WriteFlags::kCreateNew, WriteFlags::kCreateNew));
  EXPECT_FALSE(HasFlag(combined & WriteFlags::kDelete, WriteFlags::kDelete));
}

// ============================================================================
// EntityData default values
// ============================================================================

TEST(DatabaseTypes, EntityDataDefaults) {
  EntityData d;
  EXPECT_EQ(d.dbid, kInvalidDBID);
  EXPECT_EQ(d.type_id, 0u);
  EXPECT_TRUE(d.blob.empty());
  EXPECT_TRUE(d.identifier.empty());
}

// ============================================================================
// PutResult / GetResult / DelResult / LookupResult defaults
// ============================================================================

TEST(DatabaseTypes, ResultDefaults) {
  PutResult put;
  EXPECT_FALSE(put.success);
  EXPECT_EQ(put.dbid, kInvalidDBID);

  GetResult get;
  EXPECT_FALSE(get.success);
  EXPECT_FALSE(get.checked_out_by.has_value());

  DelResult del;
  EXPECT_FALSE(del.success);

  LookupResult lup;
  EXPECT_FALSE(lup.found);
  EXPECT_EQ(lup.dbid, kInvalidDBID);
}

// ============================================================================
// CheckoutInfo
// ============================================================================

TEST(DatabaseTypes, CheckoutInfoFields) {
  CheckoutInfo info;
  info.base_addr = Address(0x7F000001, 7100);
  info.app_id = 42;
  info.entity_id = 1001;

  EXPECT_EQ(info.base_addr.Port(), 7100u);
  EXPECT_EQ(info.app_id, 42u);
  EXPECT_EQ(info.entity_id, 1001u);
}

TEST(DatabaseTypes, CreateDatabaseSupportsSqlite) {
  DatabaseConfig cfg;

  auto db = CreateDatabase(cfg);
  ASSERT_NE(db, nullptr);
}

TEST(DatabaseTypes, DatabaseConfigDefaultsToSqlite) {
  DatabaseConfig cfg;
  EXPECT_EQ(cfg.type, "sqlite");
}
