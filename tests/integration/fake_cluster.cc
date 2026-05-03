#include "fake_cluster.h"

#include <cstring>

#include "baseapp/baseapp_messages.h"
#include "network/reliable_udp.h"
#include "network/socket.h"

namespace atlas::test {

namespace {

auto ReserveUdpPort() -> uint16_t {
  auto sock = Socket::CreateUdp();
  if (!sock.HasValue()) return 0;
  if (!sock->Bind(Address("127.0.0.1", 0)).HasValue()) return 0;
  auto local = sock->LocalAddress();
  return local ? local->Port() : uint16_t{0};
}

}  // namespace

FakeCluster::FakeCluster() {
  loginapp_port_ = ReserveUdpPort();
  baseapp_port_ = ReserveUdpPort();
  loginapp_addr_ = Address("127.0.0.1", loginapp_port_);
  baseapp_addr_ = Address("127.0.0.1", baseapp_port_);
  for (int i = 0; i < 32; ++i)
    expected_session_key_.bytes[i] = static_cast<uint8_t>(0xA0 + i);
}

bool FakeCluster::Start() {
  if (!net_loginapp_.StartRudpServer(loginapp_addr_)) return false;
  if (!net_baseapp_.StartRudpServer(baseapp_addr_)) return false;

  auto reg_login = net_loginapp_.InterfaceTable().RegisterTypedHandler<login::LoginRequest>(
      [this](const Address& src, Channel*, const login::LoginRequest&) {
        login_request_seen_ = true;
        if (login_policy_ == LoginPolicy::kNeverReply) return;

        auto ch_result = net_loginapp_.ConnectRudp(src, NetworkInterface::InternetRudpProfile());
        if (!ch_result) return;

        login::LoginResult reply;
        switch (login_policy_) {
          case LoginPolicy::kAccept:
            reply.status = login::LoginStatus::kSuccess;
            std::memcpy(reply.session_key.bytes, expected_session_key_.bytes, 32);
            reply.baseapp_addr = baseapp_addr_;
            break;
          case LoginPolicy::kRejectInvalidCreds:
            reply.status = login::LoginStatus::kInvalidCredentials;
            break;
          case LoginPolicy::kRejectServerFull:
            reply.status = login::LoginStatus::kServerFull;
            break;
          case LoginPolicy::kNeverReply:
            return;
        }
        (void)(*ch_result)->SendMessage(reply);
      });
  if (!reg_login) return false;

  auto reg_auth = net_baseapp_.InterfaceTable().RegisterTypedHandler<baseapp::Authenticate>(
      [this](const Address& src, Channel*, const baseapp::Authenticate& msg) {
        authenticate_request_seen_ = true;
        if (auth_policy_ == AuthPolicy::kNeverReply) return;

        auto ch_result = net_baseapp_.ConnectRudp(src, NetworkInterface::InternetRudpProfile());
        if (!ch_result) return;

        baseapp::AuthenticateResult reply;
        if (auth_policy_ == AuthPolicy::kAccept) {
          reply.success =
              std::memcmp(msg.session_key.bytes, expected_session_key_.bytes, 32) == 0;
          reply.entity_id = 42;
          reply.type_id = 7;
        } else {
          reply.success = false;
        }
        (void)(*ch_result)->SendMessage(reply);
      });
  if (!reg_auth) return false;

  auto reg_rpc = net_baseapp_.InterfaceTable().RegisterTypedHandler<baseapp::ClientBaseRpc>(
      [this](const Address&, Channel*, const baseapp::ClientBaseRpc& msg) {
        last_rpc_id_ = msg.rpc_id;
        last_rpc_payload_.assign(reinterpret_cast<const uint8_t*>(msg.payload.data()),
                                 reinterpret_cast<const uint8_t*>(msg.payload.data())
                                     + msg.payload.size());
        rpc_received_ = true;
      });
  return reg_rpc.HasValue();
}

}  // namespace atlas::test
