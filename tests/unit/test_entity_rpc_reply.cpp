#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "coro/entity_rpc_reply.h"
#include "coro/pending_rpc_registry.h"
#include "network/event_dispatcher.h"
#include "serialization/binary_stream.h"

using namespace atlas;

namespace {

constexpr int32_t kErrorTimeout = -1;

auto SerializeBody(const EntityRpcReply& reply) -> std::vector<std::byte> {
  BinaryWriter w;
  reply.Serialize(w);
  auto src = w.Data();
  return std::vector<std::byte>(src.begin(), src.end());
}

}  // namespace

TEST(EntityRpcReply, SuccessRoundTripsThroughPendingRegistry) {
  EventDispatcher dispatcher{"test"};
  PendingRpcRegistry registry{dispatcher};

  std::vector<std::byte> received;
  registry.RegisterPending(
      msg_id::Id(msg_id::Common::kEntityRpcReply), 7,
      [&](std::span<const std::byte> payload) { received.assign(payload.begin(), payload.end()); },
      [&](Error) { FAIL() << "error path should not fire"; }, Milliseconds(5000));

  // Body for the success path is a user payload — here a single int32.
  std::array<std::byte, 4> reply_body{};
  int32_t v = 42;
  std::memcpy(reply_body.data(), &v, sizeof(v));

  EntityRpcReply reply{
      .request_id = 7,
      .error_code = 0,
      .body = reply_body,
  };
  auto wire = SerializeBody(reply);

  ASSERT_TRUE(registry.TryDispatch(reply.Descriptor().id, wire));
  ASSERT_EQ(received.size(), 12u);  // [u32 req][i32 code][i32 payload]

  uint32_t header_req;
  int32_t header_code;
  int32_t payload;
  std::memcpy(&header_req, received.data(), 4);
  std::memcpy(&header_code, received.data() + 4, 4);
  std::memcpy(&payload, received.data() + 8, 4);
  EXPECT_EQ(header_req, 7u);
  EXPECT_EQ(header_code, 0);
  EXPECT_EQ(payload, 42);
}

TEST(EntityRpcReply, FailureCarriesErrorMessage) {
  EventDispatcher dispatcher{"test"};
  PendingRpcRegistry registry{dispatcher};

  std::vector<std::byte> received;
  registry.RegisterPending(
      msg_id::Id(msg_id::Common::kEntityRpcReply), 13,
      [&](std::span<const std::byte> payload) { received.assign(payload.begin(), payload.end()); },
      [&](Error) { FAIL() << "error path should not fire on dispatch"; }, Milliseconds(5000));

  BinaryWriter body_writer;
  entity_rpc_reply::WriteErrorBody(body_writer, "framework: timeout");

  auto body_data = body_writer.Data();
  EntityRpcReply reply{
      .request_id = 13,
      .error_code = kErrorTimeout,
      .body = body_data,
  };
  auto wire = SerializeBody(reply);

  ASSERT_TRUE(registry.TryDispatch(reply.Descriptor().id, wire));

  // Decode header + VLE-string error message.
  BinaryReader r{received};
  auto req = r.Read<uint32_t>();
  auto code = r.Read<int32_t>();
  auto msg = r.ReadString();
  ASSERT_TRUE(req && code && msg);
  EXPECT_EQ(*req, 13u);
  EXPECT_EQ(*code, kErrorTimeout);
  EXPECT_EQ(*msg, "framework: timeout");
}

TEST(EntityRpcReply, RegistryUnclaimedReturnsFalse) {
  EventDispatcher dispatcher{"test"};
  PendingRpcRegistry registry{dispatcher};

  EntityRpcReply reply{.request_id = 99, .error_code = 0, .body = {}};
  auto wire = SerializeBody(reply);
  EXPECT_FALSE(registry.TryDispatch(reply.Descriptor().id, wire));
}

TEST(EntityRpcReply, MessageIdInCommonRange) {
  EXPECT_GE(msg_id::Id(msg_id::Common::kEntityRpcReply), 100u);
  EXPECT_LE(msg_id::Id(msg_id::Common::kEntityRpcReply), 199u);
}
