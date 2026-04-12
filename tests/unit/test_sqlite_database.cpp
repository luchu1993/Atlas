#include "db_sqlite/sqlite_database.hpp"
#include "entitydef/entity_def_registry.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace atlas;

namespace
{

auto make_blob(std::string_view text) -> std::vector<std::byte>
{
    std::vector<std::byte> blob;
    blob.reserve(text.size());
    for (char ch : text)
    {
        blob.push_back(static_cast<std::byte>(ch));
    }
    return blob;
}

void register_account_entity()
{
    EntityDefRegistry::instance().clear();

    std::vector<std::byte> buf;
    auto write_str = [&](const std::string& s)
    {
        auto len = static_cast<uint32_t>(s.size());
        if (len < 128)
        {
            buf.push_back(static_cast<std::byte>(len));
        }
        else
        {
            buf.push_back(static_cast<std::byte>((len & 0x7F) | 0x80));
            buf.push_back(static_cast<std::byte>(len >> 7));
        }
        for (char c : s)
        {
            buf.push_back(static_cast<std::byte>(c));
        }
    };
    auto write_u8 = [&](uint8_t v) { buf.push_back(static_cast<std::byte>(v)); };
    auto write_u16 = [&](uint16_t v)
    {
        buf.push_back(static_cast<std::byte>(v & 0xFF));
        buf.push_back(static_cast<std::byte>(v >> 8));
    };

    write_str("Account");
    write_u16(1);
    write_u8(0);
    write_u8(1);
    write_u8(2);

    write_str("accountName");
    write_u8(11);
    write_u8(1);
    write_u8(1);
    write_u8(5);
    write_u16(0);
    write_u8(1);

    write_str("level");
    write_u8(6);
    write_u8(1);
    write_u8(1);
    write_u8(5);
    write_u16(1);
    write_u8(0);

    write_u8(0);

    ASSERT_TRUE(
        EntityDefRegistry::instance().register_type(buf.data(), static_cast<int32_t>(buf.size())));
}

}  // namespace

class SqliteDatabaseTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        register_account_entity();

        test_dir_ = std::filesystem::temp_directory_path() / "atlas_sqlite_db_test";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        DatabaseConfig cfg;
        cfg.type = "sqlite";
        cfg.sqlite_path = test_dir_ / "atlas.sqlite3";
        cfg.sqlite_wal = true;
        cfg.sqlite_busy_timeout_ms = 1000;
        cfg.sqlite_foreign_keys = true;

        auto start = db_.startup(cfg, EntityDefRegistry::instance());
        if (!start)
        {
            GTEST_SKIP() << "sqlite runtime unavailable: " << start.error().message();
        }
    }

    void TearDown() override
    {
        db_.shutdown();
        EntityDefRegistry::instance().clear();
        std::filesystem::remove_all(test_dir_);
    }

    SqliteDatabase db_;
    std::filesystem::path test_dir_;
};

TEST_F(SqliteDatabaseTest, PutNewEntityAndGet)
{
    PutResult put;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("hello"), "alice",
                   [&](PutResult r) { put = std::move(r); });

    ASSERT_TRUE(put.success);
    EXPECT_GT(put.dbid, 0);

    GetResult get;
    db_.get_entity(put.dbid, 1, [&](GetResult r) { get = std::move(r); });
    ASSERT_TRUE(get.success);
    EXPECT_EQ(get.data.blob, make_blob("hello"));
    EXPECT_EQ(get.data.identifier, "alice");
}

TEST_F(SqliteDatabaseTest, CheckoutAndClear)
{
    PutResult put;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("data"), "bob",
                   [&](PutResult r) { put = std::move(r); });
    ASSERT_TRUE(put.success);

    CheckoutInfo owner;
    owner.base_addr = Address(0x7F000001, 7100);
    owner.app_id = 10;
    owner.entity_id = 20;

    GetResult checkout;
    db_.checkout_entity(put.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
    ASSERT_TRUE(checkout.success);
    EXPECT_FALSE(checkout.checked_out_by.has_value());

    GetResult second;
    db_.checkout_entity(put.dbid, 1, owner, [&](GetResult r) { second = std::move(r); });
    ASSERT_TRUE(second.success);
    ASSERT_TRUE(second.checked_out_by.has_value());
    EXPECT_EQ(second.checked_out_by->app_id, 10u);

    bool cleared = false;
    db_.clear_checkout(put.dbid, 1, [&](bool ok) { cleared = ok; });
    EXPECT_TRUE(cleared);
}

TEST_F(SqliteDatabaseTest, AutoLoadAndDeferredCallbacks)
{
    db_.set_deferred_mode(true);

    PutResult put;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew | WriteFlags::AutoLoadOn,
                   make_blob("auto"), "carol", [&](PutResult r) { put = std::move(r); });

    EXPECT_FALSE(put.success);
    db_.process_results();
    ASSERT_TRUE(put.success);

    std::vector<EntityData> entities;
    db_.get_auto_load_entities([&](std::vector<EntityData> rows) { entities = std::move(rows); });
    EXPECT_TRUE(entities.empty());
    db_.process_results();
    ASSERT_EQ(entities.size(), 1u);
    EXPECT_EQ(entities[0].identifier, "carol");
}

TEST_F(SqliteDatabaseTest, LookupByNameReturnsStoredPasswordHash)
{
    PutResult put;
    db_.put_entity_with_password(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("acct"), "diana",
                                 "pw_hash_123", [&](PutResult r) { put = std::move(r); });
    ASSERT_TRUE(put.success);

    LookupResult lookup;
    db_.lookup_by_name(1, "diana", [&](LookupResult r) { lookup = std::move(r); });
    ASSERT_TRUE(lookup.found);
    EXPECT_EQ(lookup.dbid, put.dbid);
    EXPECT_EQ(lookup.password_hash, "pw_hash_123");
}

TEST_F(SqliteDatabaseTest, CheckoutByNameAndClearCheckoutsForAddress)
{
    PutResult alice;
    PutResult bob;
    PutResult chris;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("a"), "alice",
                   [&](PutResult r) { alice = std::move(r); });
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("b"), "bob",
                   [&](PutResult r) { bob = std::move(r); });
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("c"), "chris",
                   [&](PutResult r) { chris = std::move(r); });
    ASSERT_TRUE(alice.success);
    ASSERT_TRUE(bob.success);
    ASSERT_TRUE(chris.success);

    CheckoutInfo owner_a;
    owner_a.base_addr = Address(0x7F000001, 7201);
    owner_a.app_id = 21;
    owner_a.entity_id = 101;

    CheckoutInfo owner_b;
    owner_b.base_addr = Address(0x7F000001, 7202);
    owner_b.app_id = 22;
    owner_b.entity_id = 102;

    GetResult checkout_alice;
    db_.checkout_entity_by_name(1, "alice", owner_a,
                                [&](GetResult r) { checkout_alice = std::move(r); });
    ASSERT_TRUE(checkout_alice.success);
    EXPECT_FALSE(checkout_alice.checked_out_by.has_value());

    GetResult checkout_bob;
    db_.checkout_entity(bob.dbid, 1, owner_a, [&](GetResult r) { checkout_bob = std::move(r); });
    ASSERT_TRUE(checkout_bob.success);
    EXPECT_FALSE(checkout_bob.checked_out_by.has_value());

    GetResult checkout_chris;
    db_.checkout_entity(chris.dbid, 1, owner_b,
                        [&](GetResult r) { checkout_chris = std::move(r); });
    ASSERT_TRUE(checkout_chris.success);
    EXPECT_FALSE(checkout_chris.checked_out_by.has_value());

    int cleared = 0;
    db_.clear_checkouts_for_address(owner_a.base_addr, [&](int count) { cleared = count; });
    EXPECT_EQ(cleared, 2);

    GetResult alice_after_clear;
    db_.get_entity(alice.dbid, 1, [&](GetResult r) { alice_after_clear = std::move(r); });
    ASSERT_TRUE(alice_after_clear.success);
    EXPECT_FALSE(alice_after_clear.checked_out_by.has_value());

    GetResult chris_after_clear;
    db_.get_entity(chris.dbid, 1, [&](GetResult r) { chris_after_clear = std::move(r); });
    ASSERT_TRUE(chris_after_clear.success);
    ASSERT_TRUE(chris_after_clear.checked_out_by.has_value());
    EXPECT_EQ(chris_after_clear.checked_out_by->app_id, owner_b.app_id);
}

TEST_F(SqliteDatabaseTest, PutEntityWithPasswordPreservesAutoLoadAndClearsCheckoutOnLogoff)
{
    PutResult put;
    db_.put_entity_with_password(kInvalidDBID, 1, WriteFlags::CreateNew | WriteFlags::AutoLoadOn,
                                 make_blob("acct"), "eve", "pw_hash_1",
                                 [&](PutResult r) { put = std::move(r); });
    ASSERT_TRUE(put.success);

    CheckoutInfo owner;
    owner.base_addr = Address(0x7F000001, 7301);
    owner.app_id = 31;
    owner.entity_id = 301;

    GetResult checkout;
    db_.checkout_entity(put.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
    ASSERT_TRUE(checkout.success);
    EXPECT_FALSE(checkout.checked_out_by.has_value());

    PutResult update;
    db_.put_entity_with_password(put.dbid, 1, WriteFlags::LogOff, make_blob("acct_v2"), "",
                                 "pw_hash_2", [&](PutResult r) { update = std::move(r); });
    ASSERT_TRUE(update.success);
    EXPECT_EQ(update.dbid, put.dbid);

    GetResult get;
    db_.get_entity(put.dbid, 1, [&](GetResult r) { get = std::move(r); });
    ASSERT_TRUE(get.success);
    EXPECT_EQ(get.data.blob, make_blob("acct_v2"));
    EXPECT_EQ(get.data.identifier, "eve");
    EXPECT_FALSE(get.checked_out_by.has_value());

    LookupResult lookup;
    db_.lookup_by_name(1, "eve", [&](LookupResult r) { lookup = std::move(r); });
    ASSERT_TRUE(lookup.found);
    EXPECT_EQ(lookup.dbid, put.dbid);
    EXPECT_EQ(lookup.password_hash, "pw_hash_2");

    std::vector<EntityData> auto_load_entities;
    db_.get_auto_load_entities([&](std::vector<EntityData> rows)
                               { auto_load_entities = std::move(rows); });
    ASSERT_EQ(auto_load_entities.size(), 1u);
    EXPECT_EQ(auto_load_entities[0].dbid, put.dbid);
}
