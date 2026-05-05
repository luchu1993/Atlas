#ifndef ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_H_
#define ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
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

  // Send an AoI envelope to the authenticated client. reliable=true uses
  // 0xF003 (reliable enter/leave/property), reliable=false uses 0xF001
  // (volatile position update). Returns false if no auth channel yet.
  bool PushAoIEnvelope(bool reliable, std::span<const std::byte> payload);

  // Build + push the wire format produced by witness.cc::BuildEnterEnvelope:
  //   [u8 kind=1][u32 eid][u16 type_id][3f pos][3f dir][u8 og][f64 server_time][peer_snapshot]
  bool PushEntityEnter(EntityID eid, uint16_t type_id, float px, float py, float pz, float dx,
                       float dy, float dz, bool on_ground, double server_time);

  // Build + push the wire format produced by witness.cc::SendEntityUpdate:
  //   [u8 kind=3][u32 eid][3f pos][3f dir][u8 og][f64 server_time]
  bool PushEntityPositionUpdate(EntityID eid, float px, float py, float pz, float dx, float dy,
                                float dz, bool on_ground, double server_time);

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

  // Latched on the first Authenticate; reused by PushAoIEnvelope so AvatarFilter
  // tests can inject 0xF001 / 0xF003 envelopes the same way witness.cc does.
  Channel* auth_channel_{nullptr};
};

}  // namespace atlas::test

#endif  // ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_H_
