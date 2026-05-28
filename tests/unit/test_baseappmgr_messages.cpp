#include <gtest/gtest.h>

#include "baseappmgr/baseappmgr_messages.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::baseappmgr;

namespace {

template <typename Msg>
auto round_trip(const Msg& msg) -> Msg {
  BinaryWriter w;
  msg.Serialize(w);
  BinaryReader r(w.Data());
  auto result = Msg::Deserialize(r);
  EXPECT_TRUE(result.HasValue());
  return result.ValueOr(Msg{});
}

TEST(BaseAppMgrMessages, RegisterBaseApp_RoundTrip) {
  RegisterBaseApp msg;
  msg.internal_addr = Address(0x7F000001u, 9000);
  msg.external_addr = Address(0x0A000001u, 20100);

  auto out = round_trip(msg);
  EXPECT_EQ(out.internal_addr.Port(), 9000u);
  EXPECT_EQ(out.external_addr.Port(), 20100u);
}

TEST(BaseAppMgrMessages, RegisterBaseAppAck_RoundTrip) {
  RegisterBaseAppAck msg;
  msg.success = true;
  msg.app_id = 3;
  msg.game_time = 123456789u;
  msg.mgr_generation = 17u;

  auto out = round_trip(msg);
  EXPECT_TRUE(out.success);
  EXPECT_EQ(out.app_id, 3u);
  EXPECT_EQ(out.game_time, 123456789u);
  EXPECT_EQ(out.mgr_generation, 17u);
}

TEST(BaseAppMgrMessages, InformLoad_RoundTrip) {
  InformLoad msg;
  msg.app_id = 1;
  msg.load = 0.42f;
  msg.entity_count = 500;
  msg.proxy_count = 100;
  msg.pending_prepare_count = 7;
  msg.pending_force_logoff_count = 3;
  msg.detached_proxy_count = 11;
  msg.logoff_in_flight_count = 5;
  msg.deferred_login_count = 9;

  auto out = round_trip(msg);
  EXPECT_EQ(out.app_id, 1u);
  EXPECT_NEAR(out.load, 0.42f, 1e-5f);
  EXPECT_EQ(out.entity_count, 500u);
  EXPECT_EQ(out.proxy_count, 100u);
  EXPECT_EQ(out.pending_prepare_count, 7u);
  EXPECT_EQ(out.pending_force_logoff_count, 3u);
  EXPECT_EQ(out.detached_proxy_count, 11u);
  EXPECT_EQ(out.logoff_in_flight_count, 5u);
  EXPECT_EQ(out.deferred_login_count, 9u);
}

TEST(BaseAppMgrMessages, HealthProbe_RoundTrip) {
  HealthProbe msg;
  msg.nonce = 0xCAFEBABEDEADBEEFull;

  auto out = round_trip(msg);
  EXPECT_EQ(out.nonce, 0xCAFEBABEDEADBEEFull);
}

TEST(BaseAppMgrMessages, HealthProbeAck_RoundTrip) {
  HealthProbeAck msg;
  msg.nonce = 7u;
  msg.game_time = 1'234'567u;
  msg.snapshot_saves = 42u;
  msg.snapshot_failures = 1u;
  msg.snapshot_dirty = true;
  msg.snapshot_save_stale = false;

  auto out = round_trip(msg);
  EXPECT_EQ(out.nonce, 7u);
  EXPECT_EQ(out.game_time, 1'234'567u);
  EXPECT_EQ(out.snapshot_saves, 42u);
  EXPECT_EQ(out.snapshot_failures, 1u);
  EXPECT_TRUE(out.snapshot_dirty);
  EXPECT_FALSE(out.snapshot_save_stale);
}

TEST(BaseAppMgrMessages, HealthProbeAck_RejectsBadFlags) {
  // Hand-craft a payload with snapshot_dirty=2 (not 0/1) to confirm the
  // deserialiser refuses it — the Reviver heartbeat path depends on this
  // strict check to avoid garbage state being interpreted as healthy.
  BinaryWriter w;
  w.Write<uint64_t>(1);   // nonce
  w.Write<uint64_t>(0);   // game_time
  w.Write<uint64_t>(0);   // snapshot_saves
  w.Write<uint64_t>(0);   // snapshot_failures
  w.Write<uint64_t>(1);   // mgr_generation
  w.Write<uint8_t>(2);    // snapshot_dirty (invalid)
  w.Write<uint8_t>(0);    // snapshot_save_stale
  BinaryReader r(w.Data());

  auto result = HealthProbeAck::Deserialize(r);
  ASSERT_FALSE(result.HasValue());
  EXPECT_NE(std::string(result.Error().Message()).find("bad flags"), std::string::npos);
}

}  // namespace
