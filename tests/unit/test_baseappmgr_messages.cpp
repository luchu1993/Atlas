#include "baseappmgr/baseappmgr_messages.hpp"
#include "serialization/binary_stream.hpp"

#include <gtest/gtest.h>

using namespace atlas;
using namespace atlas::baseappmgr;

namespace
{

template <typename Msg>
auto round_trip(const Msg& msg) -> Msg
{
    BinaryWriter w;
    msg.serialize(w);
    BinaryReader r(w.data());
    auto result = Msg::deserialize(r);
    EXPECT_TRUE(result.has_value());
    return result.value_or(Msg{});
}

TEST(BaseAppMgrMessages, RegisterBaseApp_RoundTrip)
{
    RegisterBaseApp msg;
    msg.internal_addr = Address(0x7F000001u, 9000);
    msg.external_addr = Address(0x0A000001u, 20100);

    auto out = round_trip(msg);
    EXPECT_EQ(out.internal_addr.port(), 9000u);
    EXPECT_EQ(out.external_addr.port(), 20100u);
}

TEST(BaseAppMgrMessages, RegisterBaseAppAck_RoundTrip)
{
    RegisterBaseAppAck msg;
    msg.success = true;
    msg.app_id = 3;
    msg.entity_id_start = 10001;
    msg.entity_id_end = 20000;
    msg.game_time = 123456789u;

    auto out = round_trip(msg);
    EXPECT_TRUE(out.success);
    EXPECT_EQ(out.app_id, 3u);
    EXPECT_EQ(out.entity_id_start, 10001u);
    EXPECT_EQ(out.entity_id_end, 20000u);
}

TEST(BaseAppMgrMessages, InformLoad_RoundTrip)
{
    InformLoad msg;
    msg.app_id = 1;
    msg.load = 0.42f;
    msg.entity_count = 500;
    msg.proxy_count = 100;

    auto out = round_trip(msg);
    EXPECT_EQ(out.app_id, 1u);
    EXPECT_NEAR(out.load, 0.42f, 1e-5f);
    EXPECT_EQ(out.entity_count, 500u);
}

TEST(BaseAppMgrMessages, RegisterGlobalBase_RoundTrip)
{
    RegisterGlobalBase msg;
    msg.key = "WorldManager";
    msg.entity_id = 9999;
    msg.type_id = 5;

    auto out = round_trip(msg);
    EXPECT_EQ(out.key, "WorldManager");
    EXPECT_EQ(out.entity_id, 9999u);
}

TEST(BaseAppMgrMessages, GlobalBaseNotification_RoundTrip)
{
    GlobalBaseNotification msg;
    msg.key = "Auction";
    msg.base_addr = Address(0x01020304u, 8888);
    msg.entity_id = 42;
    msg.type_id = 7;
    msg.added = false;

    auto out = round_trip(msg);
    EXPECT_EQ(out.key, "Auction");
    EXPECT_FALSE(out.added);
    EXPECT_EQ(out.entity_id, 42u);
}

TEST(BaseAppMgrMessages, RequestEntityIdRange_RoundTrip)
{
    RequestEntityIdRange msg;
    msg.app_id = 2;

    auto out = round_trip(msg);
    EXPECT_EQ(out.app_id, 2u);
}

TEST(BaseAppMgrMessages, RequestEntityIdRangeAck_RoundTrip)
{
    RequestEntityIdRangeAck msg;
    msg.app_id = 2;
    msg.entity_id_start = 20001;
    msg.entity_id_end = 30000;

    auto out = round_trip(msg);
    EXPECT_EQ(out.entity_id_start, 20001u);
    EXPECT_EQ(out.entity_id_end, 30000u);
}

}  // namespace
