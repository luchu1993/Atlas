#include "db/database_factory.hpp"
#include "db/idatabase.hpp"

#include <gtest/gtest.h>

using namespace atlas;

// ============================================================================
// WriteFlags helpers
// ============================================================================

TEST(DatabaseTypes, WriteFlagsHasFlag)
{
    auto flags = static_cast<WriteFlags>(static_cast<uint8_t>(WriteFlags::CreateNew) |
                                         static_cast<uint8_t>(WriteFlags::AutoLoadOn));

    EXPECT_TRUE(has_flag(flags, WriteFlags::CreateNew));
    EXPECT_TRUE(has_flag(flags, WriteFlags::AutoLoadOn));
    EXPECT_FALSE(has_flag(flags, WriteFlags::LogOff));
    EXPECT_FALSE(has_flag(flags, WriteFlags::Delete));
}

TEST(DatabaseTypes, WriteFlagsAndOp)
{
    auto a = WriteFlags::CreateNew;
    auto b = WriteFlags::LogOff;
    auto combined = static_cast<WriteFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));

    EXPECT_TRUE(has_flag(combined & WriteFlags::CreateNew, WriteFlags::CreateNew));
    EXPECT_FALSE(has_flag(combined & WriteFlags::Delete, WriteFlags::Delete));
}

// ============================================================================
// EntityData default values
// ============================================================================

TEST(DatabaseTypes, EntityDataDefaults)
{
    EntityData d;
    EXPECT_EQ(d.dbid, kInvalidDBID);
    EXPECT_EQ(d.type_id, 0u);
    EXPECT_TRUE(d.blob.empty());
    EXPECT_TRUE(d.identifier.empty());
}

// ============================================================================
// PutResult / GetResult / DelResult / LookupResult defaults
// ============================================================================

TEST(DatabaseTypes, ResultDefaults)
{
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

TEST(DatabaseTypes, CheckoutInfoFields)
{
    CheckoutInfo info;
    info.base_addr = Address(0x7F000001, 7100);
    info.app_id = 42;
    info.entity_id = 1001;

    EXPECT_EQ(info.base_addr.port(), 7100u);
    EXPECT_EQ(info.app_id, 42u);
    EXPECT_EQ(info.entity_id, 1001u);
}

TEST(DatabaseTypes, CreateDatabaseSupportsSqlite)
{
    DatabaseConfig cfg;

    auto db = create_database(cfg);
    ASSERT_NE(db, nullptr);
}

TEST(DatabaseTypes, DatabaseConfigDefaultsToSqlite)
{
    DatabaseConfig cfg;
    EXPECT_EQ(cfg.type, "sqlite");
}
