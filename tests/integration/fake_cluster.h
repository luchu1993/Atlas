#ifndef ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_H_
#define ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "loginapp/login_messages.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "server/entity_types.h"

namespace atlas::test {

enum class LoginPolicy : uint8_t {
  kAccept = 0,
  kRejectInvalidCreds = 1,
  kRejectServerFull = 2,
  kNeverReply = 3,
};

enum class AuthPolicy : uint8_t {
  kAccept = 0,
  kReject = 1,
  kNeverReply = 2,
};

// Stand-in LoginApp + BaseApp for end-to-end client flow tests. Binds two
// loopback RUDP endpoints; `Pump*` from gtest / capi drives both dispatchers.
class FakeCluster {
 public:
  FakeCluster();

  bool Start();

  void SetLoginPolicy(LoginPolicy policy) { login_policy_ = policy; }
  void SetAuthPolicy(AuthPolicy policy) { auth_policy_ = policy; }

  EventDispatcher& LoginAppDisp() { return disp_loginapp_; }
  EventDispatcher& BaseAppDisp() { return disp_baseapp_; }
  uint16_t LoginAppPort() const { return loginapp_port_; }
  uint16_t BaseAppPort() const { return baseapp_port_; }

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

  LoginPolicy login_policy_{LoginPolicy::kAccept};
  AuthPolicy auth_policy_{AuthPolicy::kAccept};

  std::atomic<bool> login_request_seen_{false};
  std::atomic<bool> authenticate_request_seen_{false};
  std::atomic<bool> rpc_received_{false};
  uint32_t last_rpc_id_{0};
  std::vector<uint8_t> last_rpc_payload_;
};

}  // namespace atlas::test

#endif  // ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_H_
