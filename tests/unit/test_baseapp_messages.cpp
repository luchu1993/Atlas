#include "baseapp_messages.hpp"
#include "serialization/binary_stream.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

using namespace atlas;
using namespace atlas::baseapp;

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

TEST(BaseAppMessages, CreateBase)
{
    CreateBase msg;
    msg.type_id = 42;
    msg.entity_id = 12345;

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->type_id, 42u);
    EXPECT_EQ(rt->entity_id, 12345u);
}

TEST(BaseAppMessages, CreateBaseFromDB)
{
    CreateBaseFromDB msg;
    msg.type_id = 7;
    msg.dbid = 999;
    msg.identifier = "player_001";

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->type_id, 7u);
    EXPECT_EQ(rt->dbid, 999);
    EXPECT_EQ(rt->identifier, "player_001");
}

TEST(BaseAppMessages, AcceptClient)
{
    AcceptClient msg;
    msg.dest_entity_id = 77;
    msg.session_key.bytes[0] = 0xDE;
    msg.session_key.bytes[31] = 0xAD;

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->dest_entity_id, 77u);
    EXPECT_EQ(rt->session_key.bytes[0], 0xDE);
    EXPECT_EQ(rt->session_key.bytes[31], 0xAD);
}

TEST(BaseAppMessages, CellEntityCreated)
{
    CellEntityCreated msg;
    msg.base_entity_id = 100;
    msg.cell_entity_id = 200;
    msg.cell_addr = Address(0x7F000001u, 7002);

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->base_entity_id, 100u);
    EXPECT_EQ(rt->cell_entity_id, 200u);
    EXPECT_EQ(rt->cell_addr.port(), 7002u);
}

TEST(BaseAppMessages, CellEntityDestroyed)
{
    CellEntityDestroyed msg;
    msg.base_entity_id = 55;

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->base_entity_id, 55u);
}

TEST(BaseAppMessages, CurrentCell)
{
    CurrentCell msg;
    msg.base_entity_id = 10;
    msg.cell_entity_id = 20;
    msg.cell_addr = Address(0x0A000001u, 7003);

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->base_entity_id, 10u);
    EXPECT_EQ(rt->cell_entity_id, 20u);
    EXPECT_EQ(rt->cell_addr.port(), 7003u);
}

TEST(BaseAppMessages, CellRpcForward)
{
    CellRpcForward msg;
    msg.base_entity_id = 5;
    msg.rpc_id = 42;
    msg.payload = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->base_entity_id, 5u);
    EXPECT_EQ(rt->rpc_id, 42u);
    ASSERT_EQ(rt->payload.size(), 3u);
    EXPECT_EQ(rt->payload[1], std::byte{0xBB});
}

TEST(BaseAppMessages, SelfRpcFromCell)
{
    SelfRpcFromCell msg;
    msg.base_entity_id = 3;
    msg.rpc_id = 77;
    msg.payload = {std::byte{0x01}, std::byte{0x02}};

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->base_entity_id, 3u);
    EXPECT_EQ(rt->rpc_id, 77u);
    EXPECT_EQ(rt->payload.size(), 2u);
    EXPECT_FALSE(SelfRpcFromCell::descriptor().is_unreliable());
}

TEST(BaseAppMessages, BroadcastRpcFromCell)
{
    BroadcastRpcFromCell msg;
    msg.base_entity_id = 4;
    msg.rpc_id = 88;
    msg.target = 1;
    msg.payload = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->base_entity_id, 4u);
    EXPECT_EQ(rt->rpc_id, 88u);
    EXPECT_EQ(rt->target, 1u);
    EXPECT_EQ(rt->payload.size(), 3u);
    EXPECT_TRUE(BroadcastRpcFromCell::descriptor().is_unreliable());
}

TEST(BaseAppMessages, ReplicatedDeltaFromCell)
{
    ReplicatedDeltaFromCell msg;
    msg.base_entity_id = 8;
    msg.delta = {std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};

    auto rt = round_trip(msg);
    ASSERT_TRUE(rt.has_value());
    EXPECT_EQ(rt->base_entity_id, 8u);
    EXPECT_EQ(rt->delta.size(), 4u);
    EXPECT_EQ(rt->delta[3], std::byte{0x40});
}
