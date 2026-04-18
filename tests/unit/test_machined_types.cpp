#include <gtest/gtest.h>

#include "network/machined_types.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using namespace atlas::machined;

// Helper: serialize then deserialize
template <typename T>
static auto round_trip(const T& msg) -> Result<T> {
  BinaryWriter w;
  msg.Serialize(w);
  auto data = w.Data();
  BinaryReader r(data);
  return T::Deserialize(r);
}

// ============================================================================
// RegisterMessage
// ============================================================================

TEST(MachinedTypes, RegisterRoundTrip) {
  RegisterMessage orig;
  orig.process_type = ProcessType::kBaseApp;
  orig.name = "baseapp-1";
  orig.internal_port = 7100;
  orig.external_port = 7200;
  orig.pid = 12345;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->protocol_version, kProtocolVersion);
  EXPECT_EQ(result->process_type, ProcessType::kBaseApp);
  EXPECT_EQ(result->name, "baseapp-1");
  EXPECT_EQ(result->internal_port, 7100u);
  EXPECT_EQ(result->external_port, 7200u);
  EXPECT_EQ(result->pid, 12345u);
}

TEST(MachinedTypes, RegisterTruncatedReturnsError) {
  std::vector<std::byte> empty;
  BinaryReader r(empty);
  auto result = RegisterMessage::Deserialize(r);
  EXPECT_FALSE(result.HasValue());
}

// ============================================================================
// RegisterAck
// ============================================================================

TEST(MachinedTypes, RegisterAckSuccess) {
  RegisterAck orig;
  orig.success = true;
  orig.error_message = "";
  orig.server_time = 1234567890ULL;
  orig.heartbeat_udp_port = 0;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_TRUE(result->success);
  EXPECT_EQ(result->server_time, 1234567890ULL);
  EXPECT_EQ(result->heartbeat_udp_port, 0u);
}

TEST(MachinedTypes, RegisterAckWithUdpPort) {
  RegisterAck orig;
  orig.success = true;
  orig.error_message = "";
  orig.server_time = 9999ULL;
  orig.heartbeat_udp_port = 20019;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_TRUE(result->success);
  EXPECT_EQ(result->heartbeat_udp_port, 20019u);
}

TEST(MachinedTypes, RegisterAckFailure) {
  RegisterAck orig;
  orig.success = false;
  orig.error_message = "duplicate name";
  orig.server_time = 0;
  orig.heartbeat_udp_port = 0;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_FALSE(result->success);
  EXPECT_EQ(result->error_message, "duplicate name");
}

// ============================================================================
// DeregisterMessage
// ============================================================================

TEST(MachinedTypes, DeregisterRoundTrip) {
  DeregisterMessage orig;
  orig.process_type = ProcessType::kCellApp;
  orig.name = "cellapp-2";
  orig.pid = 99;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->process_type, ProcessType::kCellApp);
  EXPECT_EQ(result->name, "cellapp-2");
  EXPECT_EQ(result->pid, 99u);
}

// ============================================================================
// QueryMessage
// ============================================================================

TEST(MachinedTypes, QueryRoundTrip) {
  QueryMessage orig;
  orig.process_type = ProcessType::kLoginApp;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->process_type, ProcessType::kLoginApp);
}

// ============================================================================
// QueryResponse
// ============================================================================

TEST(MachinedTypes, QueryResponseEmpty) {
  QueryResponse orig;
  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_TRUE(result->processes.empty());
}

TEST(MachinedTypes, QueryResponseMultipleProcesses) {
  QueryResponse orig;
  ProcessInfo p1;
  p1.process_type = ProcessType::kBaseApp;
  p1.name = "baseapp-1";
  p1.internal_addr = Address(0x7F000001, 7100);
  p1.external_addr = Address(0, 0);
  p1.pid = 100;
  p1.load = 0.5f;

  ProcessInfo p2;
  p2.process_type = ProcessType::kCellApp;
  p2.name = "cellapp-1";
  p2.internal_addr = Address(0x7F000002, 7200);
  p2.external_addr = Address(0, 0);
  p2.pid = 200;
  p2.load = 0.0f;

  orig.processes.push_back(p1);
  orig.processes.push_back(p2);

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  ASSERT_EQ(result->processes.size(), 2u);

  EXPECT_EQ(result->processes[0].process_type, ProcessType::kBaseApp);
  EXPECT_EQ(result->processes[0].name, "baseapp-1");
  EXPECT_EQ(result->processes[0].internal_addr.Ip(), 0x7F000001u);
  EXPECT_EQ(result->processes[0].internal_addr.Port(), 7100u);
  EXPECT_EQ(result->processes[0].pid, 100u);

  EXPECT_EQ(result->processes[1].process_type, ProcessType::kCellApp);
  EXPECT_EQ(result->processes[1].pid, 200u);
}

TEST(MachinedTypes, QueryResponseCountExceedsLimit) {
  BinaryWriter w;
  w.Write<uint32_t>(10001u);  // exceeds kMaxProcesses
  auto data = w.Data();
  BinaryReader r(data);
  auto result = QueryResponse::Deserialize(r);
  EXPECT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(MachinedTypes, QueryResponseTruncatedAfterCount) {
  BinaryWriter w;
  w.Write<uint32_t>(3u);  // claims 3 entries but provides none
  auto data = w.Data();
  BinaryReader r(data);
  auto result = QueryResponse::Deserialize(r);
  EXPECT_FALSE(result.HasValue());
}

// ============================================================================
// HeartbeatMessage / HeartbeatAck
// ============================================================================

TEST(MachinedTypes, HeartbeatRoundTrip) {
  HeartbeatMessage orig;
  orig.load = 0.75f;
  orig.entity_count = 1024;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_FLOAT_EQ(result->load, 0.75f);
  EXPECT_EQ(result->entity_count, 1024u);
}

TEST(MachinedTypes, HeartbeatAckRoundTrip) {
  HeartbeatAck orig;
  orig.server_time = 9999999999ULL;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->server_time, 9999999999ULL);
}

// ============================================================================
// BirthNotification / DeathNotification
// ============================================================================

TEST(MachinedTypes, BirthNotificationRoundTrip) {
  BirthNotification orig;
  orig.process_type = ProcessType::kBaseApp;
  orig.name = "baseapp-3";
  orig.internal_addr = Address(0x7F000001, 7100);
  orig.external_addr = Address(0x01020304, 8100);
  orig.pid = 555;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->process_type, ProcessType::kBaseApp);
  EXPECT_EQ(result->name, "baseapp-3");
  EXPECT_EQ(result->internal_addr.Port(), 7100u);
  EXPECT_EQ(result->external_addr.Port(), 8100u);
  EXPECT_EQ(result->pid, 555u);
}

TEST(MachinedTypes, DeathNotificationRoundTrip) {
  DeathNotification orig;
  orig.process_type = ProcessType::kDbApp;
  orig.name = "dbapp-1";
  orig.internal_addr = Address(0x7F000001, 7300);
  orig.reason = 1;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->process_type, ProcessType::kDbApp);
  EXPECT_EQ(result->name, "dbapp-1");
  EXPECT_EQ(result->reason, 1u);
}

// ============================================================================
// ListenerRegister / ListenerAck
// ============================================================================

TEST(MachinedTypes, ListenerRegisterRoundTrip) {
  ListenerRegister orig;
  orig.listener_type = ListenerType::kBoth;
  orig.target_type = ProcessType::kBaseApp;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->listener_type, ListenerType::kBoth);
  EXPECT_EQ(result->target_type, ProcessType::kBaseApp);
}

TEST(MachinedTypes, ListenerAckRoundTrip) {
  ListenerAck orig;
  orig.success = true;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_TRUE(result->success);
}

// ============================================================================
// WatcherRequest / WatcherResponse / WatcherForward / WatcherReply
// ============================================================================

TEST(MachinedTypes, WatcherRequestRoundTrip) {
  WatcherRequest orig;
  orig.target_type = ProcessType::kBaseApp;
  orig.target_name = "baseapp-1";
  orig.watcher_path = "network/channels";
  orig.request_id = 42;

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->target_type, ProcessType::kBaseApp);
  EXPECT_EQ(result->target_name, "baseapp-1");
  EXPECT_EQ(result->watcher_path, "network/channels");
  EXPECT_EQ(result->request_id, 42u);
}

TEST(MachinedTypes, WatcherResponseRoundTrip) {
  WatcherResponse orig;
  orig.request_id = 42;
  orig.found = true;
  orig.source_name = "baseapp-1";
  orig.value = "128";

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->request_id, 42u);
  EXPECT_TRUE(result->found);
  EXPECT_EQ(result->source_name, "baseapp-1");
  EXPECT_EQ(result->value, "128");
}

TEST(MachinedTypes, WatcherForwardRoundTrip) {
  WatcherForward orig;
  orig.request_id = 7;
  orig.requester_name = "atlas_tool";
  orig.watcher_path = "server/tick_rate";

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->request_id, 7u);
  EXPECT_EQ(result->requester_name, "atlas_tool");
  EXPECT_EQ(result->watcher_path, "server/tick_rate");
}

TEST(MachinedTypes, WatcherReplyRoundTrip) {
  WatcherReply orig;
  orig.request_id = 7;
  orig.found = true;
  orig.value = "10";

  auto result = round_trip(orig);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->request_id, 7u);
  EXPECT_TRUE(result->found);
  EXPECT_EQ(result->value, "10");
}
