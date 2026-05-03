#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "fake_cluster.h"
#include "net_client/client_api.h"
#include "network/event_dispatcher.h"

using namespace atlas;
using atlas::test::FakeCluster;

namespace {

template <typename Pred>
bool PumpUntil(EventDispatcher& a, EventDispatcher& b, AtlasNetContext* ctx, Pred pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    a.ProcessOnce();
    b.ProcessOnce();
    AtlasNetPoll(ctx);
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

struct LoginCallbackState {
  std::atomic<bool> done{false};
  AtlasLoginStatus status{ATLAS_LOGIN_INTERNAL_ERROR};
  std::string baseapp_host;
  uint16_t baseapp_port{0};
};

struct AuthCallbackState {
  std::atomic<bool> done{false};
  bool success{false};
  uint32_t entity_id{0};
  uint16_t type_id{0};
};

void LoginCallback(void* user_data, uint8_t status, const char* baseapp_host, uint16_t baseapp_port,
                   const char* /*error*/) {
  auto* s = static_cast<LoginCallbackState*>(user_data);
  s->status = static_cast<AtlasLoginStatus>(status);
  if (baseapp_host) s->baseapp_host = baseapp_host;
  s->baseapp_port = baseapp_port;
  s->done = true;
}

void AuthCallback(void* user_data, uint8_t success, uint32_t entity_id, uint16_t type_id,
                  const char* /*error*/) {
  auto* s = static_cast<AuthCallbackState*>(user_data);
  s->success = success != 0;
  s->entity_id = entity_id;
  s->type_id = type_id;
  s->done = true;
}

}  // namespace

TEST(NetClientFlow, EndToEndCreateLoginAuthSendDisconnect) {
  FakeCluster cluster;
  ASSERT_TRUE(cluster.Start()) << "fake cluster failed to bind";

  AtlasNetContext* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  ASSERT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_DISCONNECTED);

  AtlasNetCallbacks cbs{};
  ASSERT_EQ(AtlasNetSetCallbacks(ctx, &cbs), ATLAS_NET_OK);

  LoginCallbackState login_state;
  ASSERT_EQ(ATLAS_NET_OK, AtlasNetLogin(ctx, "127.0.0.1", cluster.LoginAppPort(), "alice",
                                        "pwd-hash", &LoginCallback, &login_state));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx, [&] {
    return login_state.done.load();
  })) << "login callback never fired";
  EXPECT_TRUE(cluster.LoginRequestSeen());
  EXPECT_EQ(login_state.status, ATLAS_LOGIN_SUCCESS);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_LOGIN_SUCCEEDED);

  AuthCallbackState auth_state;
  ASSERT_EQ(ATLAS_NET_OK, AtlasNetAuthenticate(ctx, &AuthCallback, &auth_state));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx, [&] {
    return auth_state.done.load();
  })) << "authenticate callback never fired";
  EXPECT_TRUE(cluster.AuthenticateRequestSeen());
  EXPECT_TRUE(auth_state.success);
  EXPECT_EQ(auth_state.entity_id, 42u);
  EXPECT_EQ(auth_state.type_id, 7);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_CONNECTED);

  const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  EXPECT_EQ(ATLAS_NET_OK, AtlasNetSendBaseRpc(ctx, /*entity_id=*/42, /*rpc_id=*/0x1234, payload,
                                              static_cast<int32_t>(sizeof(payload))));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx, [&] {
    return cluster.RpcReceived();
  })) << "BaseApp never saw the RPC";
  EXPECT_EQ(cluster.LastRpcId(), 0x1234u);
  ASSERT_EQ(cluster.LastRpcPayload().size(), sizeof(payload));
  EXPECT_EQ(0, std::memcmp(cluster.LastRpcPayload().data(), payload, sizeof(payload)));

  EXPECT_EQ(ATLAS_NET_OK, AtlasNetDisconnect(ctx, ATLAS_DISCONNECT_USER));
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_DISCONNECTED);

  AtlasNetDestroy(ctx);
}

TEST(NetClientFlow, LoginToInvalidPortTimesOut) {
  AtlasNetContext* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);

  AtlasNetCallbacks cbs{};
  AtlasNetSetCallbacks(ctx, &cbs);

  LoginCallbackState login_state;
  EXPECT_EQ(ATLAS_NET_OK, AtlasNetLogin(ctx, "127.0.0.1", /*unused port*/ 1u, "alice", "pwd",
                                        &LoginCallback, &login_state));

  // Pump only the ctx; no fake cluster — the LoginApp connection times out
  // after the configured 10s deadline. We don't wait that long here; just
  // assert the state machine moved into LoggingIn and didn't return early.
  for (int i = 0; i < 100; ++i) {
    AtlasNetPoll(ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (login_state.done.load()) break;
  }
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_LOGGING_IN);

  EXPECT_EQ(ATLAS_NET_OK, AtlasNetDisconnect(ctx, ATLAS_DISCONNECT_USER));
  AtlasNetDestroy(ctx);
}
