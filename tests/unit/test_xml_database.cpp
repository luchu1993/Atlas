#include "db_xml/xml_database.hpp"
#include "entitydef/entity_def_registry.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace atlas;

// ============================================================================
// Test fixture — creates a temp directory per test
// ============================================================================

class XmlDatabaseTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Use a per-test temp dir so tests are isolated
        test_dir_ = std::filesystem::temp_directory_path() /
                    ("atlas_xml_db_test_" +
                     std::to_string(std::hash<std::string>{}(
                         ::testing::UnitTest::GetInstance()->current_test_info()->name())));
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);

        // Populate a minimal EntityDefRegistry
        EntityTypeDescriptor account_type;
        account_type.type_id = 1;
        account_type.name = "Account";
        account_type.has_cell = false;
        account_type.has_client = true;

        PropertyDescriptor name_prop;
        name_prop.name = "accountName";
        name_prop.data_type = PropertyDataType::String;
        name_prop.persistent = true;
        name_prop.identifier = true;
        name_prop.index = 0;
        account_type.properties.push_back(name_prop);

        PropertyDescriptor level_prop;
        level_prop.name = "level";
        level_prop.data_type = PropertyDataType::Int32;
        level_prop.persistent = true;
        level_prop.identifier = false;
        level_prop.index = 1;
        account_type.properties.push_back(level_prop);

        // Manually insert into a fresh registry (avoid the global singleton)
        // We use the JSON loader path is not needed here; build manually.
        // For testing we construct a local registry by registering via binary.
        // Simpler: just use XmlDatabase with the global registry after inserting there.
        // We call global instance (which is reset between test runs via clear()).
        EntityDefRegistry::instance().clear();

        // Build binary descriptor for "Account" type (mirrors register_type protocol)
        std::vector<std::byte> buf;
        auto write_str = [&](const std::string& s)
        {
            // packed_int length prefix
            auto len = static_cast<uint32_t>(s.size());
            // Simple 1-byte or 2-byte packed int
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
                buf.push_back(static_cast<std::byte>(c));
        };
        auto write_u8 = [&](uint8_t v) { buf.push_back(static_cast<std::byte>(v)); };
        auto write_u16 = [&](uint16_t v)
        {
            buf.push_back(static_cast<std::byte>(v & 0xFF));
            buf.push_back(static_cast<std::byte>(v >> 8));
        };

        write_str("Account");
        write_u16(1);  // type_id
        write_u8(0);   // has_cell
        write_u8(1);   // has_client
        write_u8(2);   // prop_count packed_int=2
        // property 0: accountName
        write_str("accountName");
        write_u8(11);  // String
        write_u8(1);   // BaseOnly
        write_u8(1);   // persistent=true
        write_u8(5);   // detail_level
        write_u16(0);  // index
        write_u8(1);   // identifier=true  (optional field)
        // property 1: level
        write_str("level");
        write_u8(6);   // Int32
        write_u8(1);   // BaseOnly
        write_u8(1);   // persistent=true
        write_u8(5);   // detail_level
        write_u16(1);  // index
        write_u8(0);   // identifier=false
        // rpc_count=0
        write_u8(0);

        ASSERT_TRUE(EntityDefRegistry::instance().register_type(buf.data(),
                                                                static_cast<int32_t>(buf.size())));

        DatabaseConfig cfg;
        cfg.type = "xml";
        cfg.xml_dir = test_dir_;

        ASSERT_TRUE(db_.startup(cfg, EntityDefRegistry::instance()).has_value());
    }

    void TearDown() override
    {
        db_.shutdown();
        std::filesystem::remove_all(test_dir_);
        EntityDefRegistry::instance().clear();
    }

    XmlDatabase db_;
    std::filesystem::path test_dir_;

    static std::vector<std::byte> make_blob(std::string_view text)
    {
        std::vector<std::byte> v;
        v.reserve(text.size());
        for (char c : text)
            v.push_back(static_cast<std::byte>(c));
        return v;
    }
};

// ============================================================================
// put (create) → get
// ============================================================================

TEST_F(XmlDatabaseTest, PutNewEntityAndGet)
{
    auto blob = make_blob("hello-world");

    PutResult put_result;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, blob, "hero123",
                   [&](PutResult r) { put_result = r; });

    ASSERT_TRUE(put_result.success);
    EXPECT_GT(put_result.dbid, 0);

    GetResult get_result;
    db_.get_entity(put_result.dbid, 1, [&](GetResult r) { get_result = std::move(r); });

    ASSERT_TRUE(get_result.success);
    EXPECT_EQ(get_result.data.blob, blob);
    EXPECT_FALSE(get_result.checked_out_by.has_value());
}

// ============================================================================
// put (update) → get
// ============================================================================

TEST_F(XmlDatabaseTest, PutUpdateEntityAndGet)
{
    auto blob1 = make_blob("v1");
    PutResult put1;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, blob1, "",
                   [&](PutResult r) { put1 = r; });
    ASSERT_TRUE(put1.success);

    auto blob2 = make_blob("v2-updated");
    PutResult put2;
    db_.put_entity(put1.dbid, 1, WriteFlags::None, blob2, "", [&](PutResult r) { put2 = r; });
    ASSERT_TRUE(put2.success);
    EXPECT_EQ(put2.dbid, put1.dbid);

    GetResult get_result;
    db_.get_entity(put1.dbid, 1, [&](GetResult r) { get_result = std::move(r); });
    ASSERT_TRUE(get_result.success);
    EXPECT_EQ(get_result.data.blob, blob2);
}

// ============================================================================
// DBID auto-increment — unique IDs
// ============================================================================

TEST_F(XmlDatabaseTest, DbidAutoIncrement)
{
    PutResult r1, r2;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("a"), "",
                   [&](PutResult r) { r1 = r; });
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("b"), "",
                   [&](PutResult r) { r2 = r; });

    ASSERT_TRUE(r1.success);
    ASSERT_TRUE(r2.success);
    EXPECT_NE(r1.dbid, r2.dbid);
}

// ============================================================================
// del → get returns error
// ============================================================================

TEST_F(XmlDatabaseTest, DeleteEntityThenGetFails)
{
    PutResult put;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("data"), "",
                   [&](PutResult r) { put = r; });
    ASSERT_TRUE(put.success);

    DelResult del;
    db_.del_entity(put.dbid, 1, [&](DelResult r) { del = r; });
    EXPECT_TRUE(del.success);

    GetResult get;
    db_.get_entity(put.dbid, 1, [&](GetResult r) { get = std::move(r); });
    EXPECT_FALSE(get.success);
}

// ============================================================================
// lookup_by_name
// ============================================================================

TEST_F(XmlDatabaseTest, LookupByName)
{
    PutResult put;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("x"), "alice",
                   [&](PutResult r) { put = r; });
    ASSERT_TRUE(put.success);

    LookupResult found;
    db_.lookup_by_name(1, "alice", [&](LookupResult r) { found = r; });
    EXPECT_TRUE(found.found);
    EXPECT_EQ(found.dbid, put.dbid);

    LookupResult not_found;
    db_.lookup_by_name(1, "nobody", [&](LookupResult r) { not_found = r; });
    EXPECT_FALSE(not_found.found);
}

// ============================================================================
// checkout / checkin
// ============================================================================

TEST_F(XmlDatabaseTest, CheckoutAndCheckin)
{
    PutResult put;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("data"), "bob",
                   [&](PutResult r) { put = r; });
    ASSERT_TRUE(put.success);

    CheckoutInfo owner;
    owner.base_addr = Address(0x7F000001, 7100);
    owner.app_id = 1;
    owner.entity_id = 101;

    GetResult co;
    db_.checkout_entity(put.dbid, 1, owner, [&](GetResult r) { co = std::move(r); });
    ASSERT_TRUE(co.success);
    EXPECT_FALSE(co.checked_out_by.has_value());  // wasn't checked out before

    // Try to checkout again — should return current owner
    GetResult co2;
    CheckoutInfo another_owner;
    another_owner.base_addr = Address(0x7F000002, 7100);
    another_owner.app_id = 2;
    another_owner.entity_id = 202;
    db_.checkout_entity(put.dbid, 1, another_owner, [&](GetResult r) { co2 = std::move(r); });
    ASSERT_TRUE(co2.success);
    ASSERT_TRUE(co2.checked_out_by.has_value());
    EXPECT_EQ(co2.checked_out_by->app_id, 1u);  // still owned by first owner

    // Clear checkout (logoff)
    bool cleared = false;
    db_.clear_checkout(put.dbid, 1, [&](bool ok) { cleared = ok; });
    EXPECT_TRUE(cleared);

    // Now checkout should succeed without existing owner
    GetResult co3;
    db_.checkout_entity(put.dbid, 1, another_owner, [&](GetResult r) { co3 = std::move(r); });
    ASSERT_TRUE(co3.success);
    EXPECT_FALSE(co3.checked_out_by.has_value());
}

TEST_F(XmlDatabaseTest, CheckoutEntityByName)
{
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("data"), "charlie",
                   [](PutResult) {});

    CheckoutInfo owner;
    owner.app_id = 5;

    GetResult co;
    db_.checkout_entity_by_name(1, "charlie", owner, [&](GetResult r) { co = std::move(r); });
    ASSERT_TRUE(co.success);
    EXPECT_GT(co.data.dbid, 0);

    GetResult not_found;
    db_.checkout_entity_by_name(1, "nobody", owner, [&](GetResult r) { not_found = std::move(r); });
    EXPECT_FALSE(not_found.success);
}

// ============================================================================
// clear_checkouts_for_address
// ============================================================================

TEST_F(XmlDatabaseTest, ClearCheckoutsForAddress)
{
    Address dead_app(0x7F000001, 7100);

    auto store_and_checkout = [&](const std::string& name, uint32_t entity_id) -> DatabaseID
    {
        PutResult put;
        db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("d"), name,
                       [&](PutResult r) { put = r; });
        if (!put.success)
            return kInvalidDBID;
        CheckoutInfo owner;
        owner.base_addr = dead_app;
        owner.entity_id = entity_id;
        db_.checkout_entity(put.dbid, 1, owner, [](GetResult) {});
        return put.dbid;
    };

    auto dbid1 = store_and_checkout("e1", 101);
    auto dbid2 = store_and_checkout("e2", 102);
    EXPECT_GT(dbid1, 0);
    EXPECT_GT(dbid2, 0);

    int cleared = 0;
    db_.clear_checkouts_for_address(dead_app, [&](int c) { cleared = c; });
    EXPECT_EQ(cleared, 2);
}

// ============================================================================
// auto_load
// ============================================================================

TEST_F(XmlDatabaseTest, AutoLoad)
{
    PutResult put;
    auto flags = WriteFlags::CreateNew | WriteFlags::AutoLoadOn;
    db_.put_entity(kInvalidDBID, 1, flags, make_blob("data"), "", [&](PutResult r) { put = r; });
    ASSERT_TRUE(put.success);

    std::vector<EntityData> entities;
    db_.get_auto_load_entities([&](std::vector<EntityData> v) { entities = std::move(v); });
    ASSERT_EQ(entities.size(), 1u);
    EXPECT_EQ(entities[0].dbid, put.dbid);

    // Clear auto-load flag
    db_.set_auto_load(put.dbid, 1, false);

    std::vector<EntityData> entities2;
    db_.get_auto_load_entities([&](std::vector<EntityData> v) { entities2 = std::move(v); });
    EXPECT_TRUE(entities2.empty());
}

// ============================================================================
// Deferred callback mode
// ============================================================================

TEST_F(XmlDatabaseTest, DeferredModeDelaysCallbacks)
{
    db_.set_deferred_mode(true);

    PutResult put_result;
    put_result.success = false;  // initial state
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("data"), "",
                   [&](PutResult r) { put_result = r; });

    // Callback not yet called
    EXPECT_FALSE(put_result.success);

    // Pump — now callback should fire
    db_.process_results();
    EXPECT_TRUE(put_result.success);
    EXPECT_GT(put_result.dbid, 0);

    db_.set_deferred_mode(false);
}

// ============================================================================
// WriteFlags::LogOff clears checkout
// ============================================================================

TEST_F(XmlDatabaseTest, PutWithLogOffClearsCheckout)
{
    PutResult put;
    db_.put_entity(kInvalidDBID, 1, WriteFlags::CreateNew, make_blob("data"), "dave",
                   [&](PutResult r) { put = r; });
    ASSERT_TRUE(put.success);

    CheckoutInfo owner;
    owner.app_id = 3;
    db_.checkout_entity(put.dbid, 1, owner, [](GetResult) {});

    // Verify checked out
    GetResult g1;
    db_.get_entity(put.dbid, 1, [&](GetResult r) { g1 = std::move(r); });
    EXPECT_TRUE(g1.checked_out_by.has_value());

    // put with LogOff should clear checkout
    db_.put_entity(put.dbid, 1, WriteFlags::LogOff, make_blob("updated"), "", [](PutResult) {});

    GetResult g2;
    db_.get_entity(put.dbid, 1, [&](GetResult r) { g2 = std::move(r); });
    EXPECT_FALSE(g2.checked_out_by.has_value());
}
