#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "baseapp/baseapp_messages.h"
#include "loginapp/login_messages.h"
#include "net_client/client_api.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "network/socket.h"

using namespace atlas;

namespace {

auto ReserveUdpPort() -> uint16_t {
  auto sock = Socket::CreateUdp();
  EXPECT_TRUE(sock.HasValue());
  EXPECT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());
  auto local = sock->LocalAddress();
  return local ? local->Port() : uint16_t{0};
}

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

class FakeCluster {
 public:
  FakeCluster() {
    loginapp_port_ = ReserveUdpPort();
    baseapp_port_ = ReserveUdpPort();
    loginapp_addr_ = Address("127.0.0.1", loginapp_port_);
    baseapp_addr_ = Address("127.0.0.1", baseapp_port_);

    for (int i = 0; i < 32; ++i) expected_session_key_.bytes[i] = static_cast<uint8_t>(0xA0 + i);
  }

  bool Start() {
    auto la = net_loginapp_.StartRudpServer(loginapp_addr_);
    if (!la) return false;
    auto ba = net_baseapp_.StartRudpServer(baseapp_addr_);
    if (!ba) return false;

    auto reg_login = net_loginapp_.InterfaceTable().RegisterTypedHandler<login::LoginRequest>(
        [this](const Address& src, Channel*, const login::LoginRequest& msg) {
          last_login_username_ = msg.username;
          last_login_password_hash_ = msg.password_hash;
          login_request_seen_ = true;

          auto ch_result = net_loginapp_.ConnectRudp(src, NetworkInterface::InternetRudpProfile());
          if (!ch_result) return;
          login::LoginResult reply;
          reply.status = login::LoginStatus::kSuccess;
          std::memcpy(reply.session_key.bytes, expected_session_key_.bytes, 32);
          reply.baseapp_addr = baseapp_addr_;
          (void)(*ch_result)->SendMessage(reply);
        });
    if (!reg_login) return false;

    auto reg_auth = net_baseapp_.InterfaceTable().RegisterTypedHandler<baseapp::Authenticate>(
        [this](const Address& src, Channel*, const baseapp::Authenticate& msg) {
          authenticate_request_seen_ = true;
          baseapp::AuthenticateResult reply;
          reply.success = std::memcmp(msg.session_key.bytes, expected_session_key_.bytes, 32) == 0;
          reply.entity_id = 42;
          reply.type_id = 7;
          auto ch_result = net_baseapp_.ConnectRudp(src, NetworkInterface::InternetRudpProfile());
          if (!ch_result) return;
          (void)(*ch_result)->SendMessage(reply);
        });
    if (!reg_auth) return false;

    auto reg_rpc = net_baseapp_.InterfaceTable().RegisterTypedHandler<baseapp::ClientBaseRpc>(
        [this](const Address&, Channel*, const baseapp::ClientBaseRpc& msg) {
          last_rpc_id_ = msg.rpc_id;
          last_rpc_payload_ = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(msg.payload.data()),
                                                   reinterpret_cast<const uint8_t*>(msg.payload.data())
                                                       + msg.payload.size());
          rpc_received_ = true;
        });
    return reg_rpc.HasValue();
  }

  EventDispatcher& LoginAppDisp() { return disp_loginapp_; }
  EventDispatcher& BaseAppDisp() { return disp_baseapp_; }
  uint16_t LoginAppPort() const { return loginapp_port_; }

  bool LoginRequestSeen() const { return login_request_seen_; }
  bool AuthenticateRequestSeen() const { return authenticate_request_seen_; }
  bool RpcReceived() const { return rpc_received_; }
  uint32_t LastRpcId() const { return last_rpc_id_; }
  const std::vector<uint8_t>& LastRpcPayload() const { return last_rpc_payload_; }

 private:
  EventDispatcher disp_loginapp_{"fake_loginapp"};
  NetworkInterface net_loginapp_{disp_loginapp_};
  EventDispatcher disp_baseapp_{"fake_baseapp"};
  NetworkInterface net_baseapp_{disp_baseapp_};

  uint16_t loginapp_port_{0};
  uint16_t baseapp_port_{0};
  Address loginapp_addr_;
  Address baseapp_addr_;
  SessionKey expected_session_key_{};

  std::atomic<bool> login_request_seen_{false};
  std::atomic<bool> authenticate_request_seen_{false};
  std::atomic<bool> rpc_received_{false};
  std::string last_login_username_;
  std::string last_login_password_hash_;
  uint32_t last_rpc_id_{0};
  std::vector<uint8_t> last_rpc_payload_;
};

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

void LoginCallback(void* user_data, uint8_t status,
                   const char* baseapp_host, uint16_t baseapp_port,
                   const char* /*error*/) {
  auto* s = static_cast<LoginCallbackState*>(user_data);
  s->status = static_cast<AtlasLoginStatus>(status);
  if (baseapp_host) s->baseapp_host = baseapp_host;
  s->baseapp_port = baseapp_port;
  s->done = true;
}

void AuthCallback(void* user_data, uint8_t success,
                  uint32_t entity_id, uint16_t type_id,
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
  ASSERT_EQ(ATLAS_NET_OK,
            AtlasNetLogin(ctx, "127.0.0.1", cluster.LoginAppPort(),
                          "alice", "pwd-hash",
                          &LoginCallback, &login_state));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return login_state.done.load(); }))
      << "login callback never fired";
  EXPECT_TRUE(cluster.LoginRequestSeen());
  EXPECT_EQ(login_state.status, ATLAS_LOGIN_SUCCESS);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_LOGIN_SUCCEEDED);

  AuthCallbackState auth_state;
  ASSERT_EQ(ATLAS_NET_OK, AtlasNetAuthenticate(ctx, &AuthCallback, &auth_state));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return auth_state.done.load(); }))
      << "authenticate callback never fired";
  EXPECT_TRUE(cluster.AuthenticateRequestSeen());
  EXPECT_TRUE(auth_state.success);
  EXPECT_EQ(auth_state.entity_id, 42u);
  EXPECT_EQ(auth_state.type_id, 7);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_CONNECTED);

  const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  EXPECT_EQ(ATLAS_NET_OK,
            AtlasNetSendBaseRpc(ctx, /*entity_id=*/42, /*rpc_id=*/0x1234,
                                payload, static_cast<int32_t>(sizeof(payload))));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return cluster.RpcReceived(); }))
      << "BaseApp never saw the RPC";
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
  EXPECT_EQ(ATLAS_NET_OK,
            AtlasNetLogin(ctx, "127.0.0.1", /*unused port*/ 1u,
                          "alice", "pwd",
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
