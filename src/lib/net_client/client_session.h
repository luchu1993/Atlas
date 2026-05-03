#ifndef ATLAS_LIB_NET_CLIENT_CLIENT_SESSION_H_
#define ATLAS_LIB_NET_CLIENT_CLIENT_SESSION_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "foundation/timer_queue.h"
#include "net_client/client_api.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "server/entity_types.h"

namespace atlas::login {
struct LoginResult;
}
namespace atlas::baseapp {
struct AuthenticateResult;
}

namespace atlas::net_client {

// Async Login/Auth state machine for the Unity DLL. Single-thread-owned.
// Replaces the blocking poll loop in src/client/client_app.cc.
class ClientSession {
 public:
  ClientSession();
  ~ClientSession();

  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;

  auto Poll() -> int32_t;

  [[nodiscard]] auto GetState() const -> AtlasNetState { return state_; }

  // Legal only in DISCONNECTED.
  auto StartLogin(std::string_view loginapp_host, uint16_t loginapp_port, std::string_view username,
                  std::string_view password_hash, AtlasLoginResultFn callback, void* user_data)
      -> int32_t;

  // Legal only in LOGIN_SUCCEEDED. Reads cached SessionKey + BaseApp addr.
  auto StartAuthenticate(AtlasAuthResultFn callback, void* user_data) -> int32_t;

  // Idempotent. LOGOUT fires on_disconnect with reason=3; USER is silent.
  auto Disconnect(AtlasDisconnectReason reason) -> int32_t;

  auto SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len)
      -> int32_t;

  auto SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len)
      -> int32_t;

  // NULL fields in `cb` are substituted with internal noops.
  auto SetCallbacks(const AtlasNetCallbacks& cb) -> int32_t;

  [[nodiscard]] auto LastError() const -> const std::string& { return last_error_; }

  auto FillStats(AtlasNetStats* out) const -> int32_t;

 private:
  void OnLoginResult(const ::atlas::login::LoginResult& msg);
  void FireLoginCallback(AtlasLoginStatus status, std::string_view baseapp_host,
                         uint16_t baseapp_port, std::string_view error);
  void CancelLoginTimeout();

  void OnAuthResult(const ::atlas::baseapp::AuthenticateResult& msg);
  void FireAuthCallback(bool success, EntityID entity_id, uint16_t type_id, std::string_view error);
  void CancelAuthTimeout();

  void InstallDefaultHandler();
  void CloseLoginAppChannel();
  void CloseBaseAppChannel();
  void ClearSessionKey();

  EventDispatcher dispatcher_{"net_client"};
  NetworkInterface network_{dispatcher_};

  AtlasNetState state_{ATLAS_NET_STATE_DISCONNECTED};

  Channel* loginapp_channel_{nullptr};
  Channel* baseapp_channel_{nullptr};

  std::array<uint8_t, sizeof(SessionKey::bytes)> session_key_{};
  Address baseapp_addr_{};
  EntityID player_entity_id_{kInvalidEntityID};
  uint16_t player_type_id_{0};

  AtlasLoginResultFn login_callback_{nullptr};
  void* login_user_data_{nullptr};
  TimerHandle login_timeout_{};

  AtlasAuthResultFn auth_callback_{nullptr};
  void* auth_user_data_{nullptr};
  TimerHandle auth_timeout_{};

  AtlasNetCallbacks callbacks_{};

  std::string last_error_;

  static constexpr Duration kLoginTimeout{std::chrono::seconds(10)};
  static constexpr Duration kAuthTimeout{std::chrono::seconds(5)};
};

}  // namespace atlas::net_client

struct AtlasNetContext : ::atlas::net_client::ClientSession {};

#endif  // ATLAS_LIB_NET_CLIENT_CLIENT_SESSION_H_
