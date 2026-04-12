#include "dbapp_messages.hpp"
#include "serialization/binary_stream.hpp"

#include <gtest/gtest.h>

using namespace atlas;
using namespace atlas::dbapp;

namespace
{

// Helper: serialise a message into a buffer and deserialise it back.
template <typename Msg>
auto round_trip(const Msg& msg) -> std::optional<Msg>
{
    BinaryWriter w;
    msg.serialize(w);
    auto buf = w.detach();

    BinaryReader r(buf);
    auto result = Msg::deserialize(r);
    if (!result)
        return std::nullopt;
    return std::move(*result);
}

std::vector<std::byte> make_blob(std::string_view text)
{
    std::vector<std::byte> v;
    v.reserve(text.size());
    for (char c : text)
        v.push_back(static_cast<std::byte>(c));
    return v;
}

}  // namespace

// ============================================================================
// WriteEntity
// ============================================================================

TEST(DbappMessages, WriteEntityRoundTrip)
{
    WriteEntity msg;
    msg.flags = WriteFlags::CreateNew | WriteFlags::AutoLoadOn;
    msg.type_id = 7;
    msg.dbid = kInvalidDBID;
    msg.entity_id = 42;
    msg.request_id = 999;
    msg.identifier = "hero_test";
    msg.blob = make_blob("blob-payload");

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(static_cast<uint8_t>(out->flags),
              static_cast<uint8_t>(WriteFlags::CreateNew | WriteFlags::AutoLoadOn));
    EXPECT_EQ(out->type_id, 7u);
    EXPECT_EQ(out->dbid, kInvalidDBID);
    EXPECT_EQ(out->entity_id, 42u);
    EXPECT_EQ(out->request_id, 999u);
    EXPECT_EQ(out->identifier, "hero_test");
    EXPECT_EQ(out->blob, msg.blob);
}

TEST(DbappMessages, WriteEntityEmptyBlobAndIdentifier)
{
    WriteEntity msg;
    msg.type_id = 1;
    msg.dbid = 100;
    msg.request_id = 1;
    // blob and identifier empty

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->blob.empty());
    EXPECT_TRUE(out->identifier.empty());
    EXPECT_EQ(out->dbid, 100);
}

// ============================================================================
// WriteEntityAck
// ============================================================================

TEST(DbappMessages, WriteEntityAckSuccess)
{
    WriteEntityAck msg;
    msg.request_id = 77;
    msg.success = true;
    msg.dbid = 42;
    msg.error = "";

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->request_id, 77u);
    EXPECT_TRUE(out->success);
    EXPECT_EQ(out->dbid, 42);
    EXPECT_TRUE(out->error.empty());
}

TEST(DbappMessages, WriteEntityAckFailure)
{
    WriteEntityAck msg;
    msg.request_id = 78;
    msg.success = false;
    msg.dbid = kInvalidDBID;
    msg.error = "disk full";

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->success);
    EXPECT_EQ(out->error, "disk full");
}

// ============================================================================
// CheckoutEntity
// ============================================================================

TEST(DbappMessages, CheckoutEntityByName)
{
    CheckoutEntity msg;
    msg.mode = LoadMode::ByName;
    msg.type_id = 3;
    msg.dbid = kInvalidDBID;
    msg.identifier = "alice";
    msg.entity_id = 55;
    msg.request_id = 100;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->mode, LoadMode::ByName);
    EXPECT_EQ(out->type_id, 3u);
    EXPECT_EQ(out->identifier, "alice");
    EXPECT_EQ(out->entity_id, 55u);
    EXPECT_EQ(out->request_id, 100u);
}

TEST(DbappMessages, CheckoutEntityByDBID)
{
    CheckoutEntity msg;
    msg.mode = LoadMode::ByDBID;
    msg.type_id = 2;
    msg.dbid = 999;
    msg.entity_id = 10;
    msg.request_id = 200;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->mode, LoadMode::ByDBID);
    EXPECT_EQ(out->dbid, 999);
}

// ============================================================================
// CheckoutEntityAck
// ============================================================================

TEST(DbappMessages, CheckoutEntityAckSuccess)
{
    CheckoutEntityAck msg;
    msg.request_id = 101;
    msg.status = CheckoutStatus::Success;
    msg.dbid = 42;
    msg.blob = make_blob("entity-data");
    msg.holder_addr = Address(0, 0);
    msg.holder_app_id = 0;
    msg.holder_entity_id = 0;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->request_id, 101u);
    EXPECT_EQ(out->status, CheckoutStatus::Success);
    EXPECT_EQ(out->dbid, 42);
    EXPECT_EQ(out->blob, msg.blob);
}

TEST(DbappMessages, CheckoutEntityAckAlreadyCheckedOut)
{
    CheckoutEntityAck msg;
    msg.request_id = 102;
    msg.status = CheckoutStatus::AlreadyCheckedOut;
    msg.dbid = 42;
    msg.holder_addr = Address(0x7F000001, 7100);
    msg.holder_app_id = 3;
    msg.holder_entity_id = 101;
    msg.error = "already checked out";

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->status, CheckoutStatus::AlreadyCheckedOut);
    EXPECT_EQ(out->holder_addr.ip(), 0x7F000001u);
    EXPECT_EQ(out->holder_addr.port(), 7100u);
    EXPECT_EQ(out->holder_app_id, 3u);
    EXPECT_EQ(out->error, "already checked out");
}

// ============================================================================
// CheckinEntity
// ============================================================================

TEST(DbappMessages, CheckinEntityRoundTrip)
{
    CheckinEntity msg;
    msg.type_id = 5;
    msg.dbid = 77;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->type_id, 5u);
    EXPECT_EQ(out->dbid, 77);
}

// ============================================================================
// DeleteEntity / DeleteEntityAck
// ============================================================================

TEST(DbappMessages, DeleteEntityRoundTrip)
{
    DeleteEntity msg;
    msg.type_id = 1;
    msg.dbid = 50;
    msg.request_id = 300;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->type_id, 1u);
    EXPECT_EQ(out->dbid, 50);
    EXPECT_EQ(out->request_id, 300u);
}

TEST(DbappMessages, DeleteEntityAckRoundTrip)
{
    DeleteEntityAck msg;
    msg.request_id = 300;
    msg.success = true;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->success);
    EXPECT_TRUE(out->error.empty());
}

// ============================================================================
// LookupEntity / LookupEntityAck
// ============================================================================

TEST(DbappMessages, LookupEntityRoundTrip)
{
    LookupEntity msg;
    msg.type_id = 2;
    msg.identifier = "bob";
    msg.request_id = 400;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->type_id, 2u);
    EXPECT_EQ(out->identifier, "bob");
    EXPECT_EQ(out->request_id, 400u);
}

TEST(DbappMessages, LookupEntityAckFound)
{
    LookupEntityAck msg;
    msg.request_id = 400;
    msg.found = true;
    msg.dbid = 88;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->found);
    EXPECT_EQ(out->dbid, 88);
}

TEST(DbappMessages, LookupEntityAckNotFound)
{
    LookupEntityAck msg;
    msg.request_id = 401;
    msg.found = false;
    msg.dbid = kInvalidDBID;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(out->found);
    EXPECT_EQ(out->dbid, kInvalidDBID);
}

TEST(DbappMessages, AbortCheckoutRoundTrip)
{
    AbortCheckout msg;
    msg.request_id = 501;
    msg.type_id = 9;
    msg.dbid = 12345;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->request_id, 501u);
    EXPECT_EQ(out->type_id, 9u);
    EXPECT_EQ(out->dbid, 12345);
}

TEST(DbappMessages, AbortCheckoutAckRoundTrip)
{
    AbortCheckoutAck msg;
    msg.request_id = 501;
    msg.success = true;

    auto out = round_trip(msg);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->request_id, 501u);
    EXPECT_TRUE(out->success);
}

// ============================================================================
// Descriptor IDs
// ============================================================================

TEST(DbappMessages, MessageDescriptorIds)
{
    EXPECT_EQ(WriteEntity::descriptor().id, 4000);
    EXPECT_EQ(WriteEntityAck::descriptor().id, 4001);
    EXPECT_EQ(CheckoutEntity::descriptor().id, 4002);
    EXPECT_EQ(CheckoutEntityAck::descriptor().id, 4003);
    EXPECT_EQ(CheckinEntity::descriptor().id, 4004);
    EXPECT_EQ(DeleteEntity::descriptor().id, 4005);
    EXPECT_EQ(DeleteEntityAck::descriptor().id, 4006);
    EXPECT_EQ(LookupEntity::descriptor().id, 4007);
    EXPECT_EQ(LookupEntityAck::descriptor().id, 4008);
    EXPECT_EQ(AbortCheckout::descriptor().id, 4009);
    EXPECT_EQ(AbortCheckoutAck::descriptor().id, 4010);
}
