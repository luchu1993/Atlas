#include "net_client/client_session.h"

#include <cstring>
#include <format>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "foundation/log.h"
#include "loginapp/login_messages.h"
#include "network/channel.h"
#include "network/interface_table.h"
#include "network/reliable_udp.h"
#include "serialization/binary_stream.h"

namespace atlas::net_client {

namespace {

void SecureZero(void* p, std::size_t n) {
  volatile auto* vp = static_cast<volatile unsigned char*>(p);
  while (n--) *vp++ = 0;
}

constexpr int32_t kRpcReason_LoggedOff = 3;

auto MapLoginStatus(::atlas::login::LoginStatus status) -> AtlasLoginStatus {
  using ::atlas::login::LoginStatus;
  switch (status) {
    case LoginStatus::kSuccess:             return ATLAS_LOGIN_SUCCESS;
    case LoginStatus::kInvalidCredentials:  return ATLAS_LOGIN_INVALID_CREDENTIALS;
    case LoginStatus::kAlreadyLoggedIn:     return ATLAS_LOGIN_ALREADY_LOGGED_IN;
    case LoginStatus::kServerFull:          return ATLAS_LOGIN_SERVER_FULL;
    case LoginStatus::kRateLimited:
    case LoginStatus::kServerNotReady:
    case LoginStatus::kServerBusy:          return ATLAS_LOGIN_SERVER_FULL;
    case LoginStatus::kInternalError:
    case LoginStatus::kLoginInProgress:     return ATLAS_LOGIN_INTERNAL_ERROR;
  }
  return ATLAS_LOGIN_INTERNAL_ERROR;
}

auto FormatHostOnly(const Address& addr) -> std::string {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&addr);
  return std::format("{}.{}.{}.{}", bytes[0], bytes[1], bytes[2], bytes[3]);
}

}  // namespace

ClientSession::ClientSession() = default;

ClientSession::~ClientSession() {
  if (state_ != ATLAS_NET_STATE_DISCONNECTED) {
    Disconnect(ATLAS_DISCONNECT_INTERNAL);
  }
  ClearSessionKey();
}

auto ClientSession::Poll() -> int32_t {
  dispatcher_.ProcessOnce();
  return 0;
}

auto ClientSession::StartLogin(std::string_view loginapp_host,
                               uint16_t loginapp_port,
                               std::string_view username,
                               std::string_view password_hash,
                               AtlasLoginResultFn callback,
                               void* user_data) -> int32_t {
  if (state_ != ATLAS_NET_STATE_DISCONNECTED) {
    last_error_ = "AtlasNetLogin requires Disconnected state";
    return ATLAS_NET_ERR_BUSY;
  }
  if (loginapp_host.empty() || loginapp_port == 0 || !callback) {
    last_error_ = "AtlasNetLogin: invalid args";
    return ATLAS_NET_ERR_INVAL;
  }

  Address login_addr(loginapp_host, loginapp_port);
  auto ch_result =
      network_.ConnectRudp(login_addr, NetworkInterface::InternetRudpProfile());
  if (!ch_result) {
    last_error_ = std::format("ConnectRudp(loginapp) failed: {}",
                              ch_result.Error().Message());
    ATLAS_LOG_ERROR("ClientSession: {}", last_error_);
    return ATLAS_NET_ERR_NOCONN;
  }
  loginapp_channel_ = *ch_result;

  auto reg = network_.InterfaceTable().RegisterTypedHandler<::atlas::login::LoginResult>(
      [this](const Address&, Channel*, const ::atlas::login::LoginResult& msg) {
        OnLoginResult(msg);
      });
  if (!reg) {
    last_error_ = std::format("Register LoginResult handler failed: {}",
                              reg.Error().Message());
    CloseLoginAppChannel();
    return ATLAS_NET_ERR_INVAL;
  }

  ::atlas::login::LoginRequest req;
  req.username = std::string(username);
  req.password_hash = std::string(password_hash);
  (void)loginapp_channel_->SendMessage(req);

  login_callback_ = callback;
  login_user_data_ = user_data;
  state_ = ATLAS_NET_STATE_LOGGING_IN;

  login_timeout_ = dispatcher_.AddTimer(kLoginTimeout, [this](TimerHandle) {
    if (state_ != ATLAS_NET_STATE_LOGGING_IN) return;
    last_error_ = "Login timeout";
    ATLAS_LOG_WARNING("ClientSession: login timed out");
    CloseLoginAppChannel();
    state_ = ATLAS_NET_STATE_DISCONNECTED;
    FireLoginCallback(ATLAS_LOGIN_TIMEOUT, {}, 0, last_error_);
  });

  return ATLAS_NET_OK;
}

void ClientSession::OnLoginResult(const ::atlas::login::LoginResult& msg) {
  if (state_ != ATLAS_NET_STATE_LOGGING_IN) return;
  CancelLoginTimeout();

  if (msg.status != ::atlas::login::LoginStatus::kSuccess) {
    last_error_ = msg.error_message.empty() ? "login rejected" : msg.error_message;
    CloseLoginAppChannel();
    state_ = ATLAS_NET_STATE_DISCONNECTED;
    FireLoginCallback(MapLoginStatus(msg.status), {}, 0, last_error_);
    return;
  }

  std::memcpy(session_key_.data(), msg.session_key.bytes, session_key_.size());
  baseapp_addr_ = msg.baseapp_addr;

  CloseLoginAppChannel();
  state_ = ATLAS_NET_STATE_LOGIN_SUCCEEDED;

  std::string host_str = FormatHostOnly(baseapp_addr_);
  FireLoginCallback(ATLAS_LOGIN_SUCCESS, host_str, baseapp_addr_.Port(), {});
}

void ClientSession::FireLoginCallback(AtlasLoginStatus status,
                                      std::string_view host,
                                      uint16_t port,
                                      std::string_view error) {
  auto cb = std::exchange(login_callback_, nullptr);
  void* ud = std::exchange(login_user_data_, nullptr);
  if (!cb) return;

  std::string host_buf{host};
  std::string err_buf{error};
  cb(ud, static_cast<uint8_t>(status),
     status == ATLAS_LOGIN_SUCCESS ? host_buf.c_str() : nullptr,
     status == ATLAS_LOGIN_SUCCESS ? port : static_cast<uint16_t>(0),
     err_buf.empty() ? nullptr : err_buf.c_str());
}

void ClientSession::CancelLoginTimeout() {
  if (login_timeout_.IsValid()) {
    dispatcher_.CancelTimer(login_timeout_);
    login_timeout_ = TimerHandle{};
  }
}

auto ClientSession::StartAuthenticate(AtlasAuthResultFn callback,
                                      void* user_data) -> int32_t {
  if (state_ != ATLAS_NET_STATE_LOGIN_SUCCEEDED) {
    last_error_ = "AtlasNetAuthenticate requires LoginSucceeded state";
    return ATLAS_NET_ERR_BUSY;
  }
  if (!callback) {
    last_error_ = "AtlasNetAuthenticate: callback is NULL";
    return ATLAS_NET_ERR_INVAL;
  }

  auto ch_result = network_.ConnectRudp(baseapp_addr_,
                                        NetworkInterface::InternetRudpProfile());
  if (!ch_result) {
    last_error_ = std::format("ConnectRudp(baseapp) failed: {}",
                              ch_result.Error().Message());
    ATLAS_LOG_ERROR("ClientSession: {}", last_error_);
    ClearSessionKey();
    state_ = ATLAS_NET_STATE_DISCONNECTED;
    return ATLAS_NET_ERR_NOCONN;
  }
  baseapp_channel_ = *ch_result;

  auto reg = network_.InterfaceTable()
                 .RegisterTypedHandler<::atlas::baseapp::AuthenticateResult>(
                     [this](const Address&, Channel*,
                            const ::atlas::baseapp::AuthenticateResult& msg) {
                       OnAuthResult(msg);
                     });
  if (!reg) {
    last_error_ = std::format("Register AuthenticateResult handler failed: {}",
                              reg.Error().Message());
    CloseBaseAppChannel();
    ClearSessionKey();
    state_ = ATLAS_NET_STATE_DISCONNECTED;
    return ATLAS_NET_ERR_INVAL;
  }

  ::atlas::baseapp::Authenticate auth_msg;
  std::memcpy(auth_msg.session_key.bytes, session_key_.data(), session_key_.size());
  (void)baseapp_channel_->SendMessage(auth_msg);

  auth_callback_ = callback;
  auth_user_data_ = user_data;
  state_ = ATLAS_NET_STATE_AUTHENTICATING;

  auth_timeout_ = dispatcher_.AddTimer(kAuthTimeout, [this](TimerHandle) {
    if (state_ != ATLAS_NET_STATE_AUTHENTICATING) return;
    last_error_ = "Authentication timeout";
    ATLAS_LOG_WARNING("ClientSession: auth timed out");
    CloseBaseAppChannel();
    ClearSessionKey();
    state_ = ATLAS_NET_STATE_DISCONNECTED;
    FireAuthCallback(false, kInvalidEntityID, 0, last_error_);
  });

  return ATLAS_NET_OK;
}

void ClientSession::OnAuthResult(const ::atlas::baseapp::AuthenticateResult& msg) {
  if (state_ != ATLAS_NET_STATE_AUTHENTICATING) return;
  CancelAuthTimeout();

  if (!msg.success) {
    last_error_ = msg.error.empty() ? "authentication rejected" : msg.error;
    CloseBaseAppChannel();
    ClearSessionKey();
    state_ = ATLAS_NET_STATE_DISCONNECTED;
    FireAuthCallback(false, kInvalidEntityID, 0, last_error_);
    return;
  }

  player_entity_id_ = msg.entity_id;
  player_type_id_ = msg.type_id;
  state_ = ATLAS_NET_STATE_CONNECTED;

  InstallDefaultHandler();
  FireAuthCallback(true, player_entity_id_, player_type_id_, {});
}

void ClientSession::FireAuthCallback(bool success, EntityID eid, uint16_t tid,
                                     std::string_view error) {
  auto cb = std::exchange(auth_callback_, nullptr);
  void* ud = std::exchange(auth_user_data_, nullptr);
  if (!cb) return;

  std::string err_buf{error};
  cb(ud, success ? uint8_t{1} : uint8_t{0}, eid, tid,
     err_buf.empty() ? nullptr : err_buf.c_str());
}

void ClientSession::CancelAuthTimeout() {
  if (auth_timeout_.IsValid()) {
    dispatcher_.CancelTimer(auth_timeout_);
    auth_timeout_ = TimerHandle{};
  }
}

void ClientSession::InstallDefaultHandler() {
  network_.InterfaceTable().SetDefaultHandler(
      [this](const Address&, Channel*, MessageID id, BinaryReader& reader) {
        const uint8_t* payload = nullptr;
        int32_t len = 0;
        const auto remaining = reader.Remaining();
        if (remaining > 0) {
          auto bytes_result = reader.ReadBytes(remaining);
          if (bytes_result) {
            payload = reinterpret_cast<const uint8_t*>(bytes_result->data());
            len = static_cast<int32_t>(bytes_result->size());
          }
        }
        callbacks_.on_rpc(reinterpret_cast<AtlasNetContext*>(this),
                          player_entity_id_, static_cast<uint32_t>(id),
                          payload, len);
      });
}

auto ClientSession::Disconnect(AtlasDisconnectReason reason) -> int32_t {
  const bool was_connected = state_ != ATLAS_NET_STATE_DISCONNECTED;

  CancelLoginTimeout();
  CancelAuthTimeout();
  CloseLoginAppChannel();
  CloseBaseAppChannel();
  ClearSessionKey();
  network_.InterfaceTable().SetDefaultHandler(nullptr);

  state_ = ATLAS_NET_STATE_DISCONNECTED;
  player_entity_id_ = kInvalidEntityID;
  player_type_id_ = 0;

  if (reason == ATLAS_DISCONNECT_LOGOUT && was_connected) {
    callbacks_.on_disconnect(reinterpret_cast<AtlasNetContext*>(this),
                             kRpcReason_LoggedOff);
  }
  return ATLAS_NET_OK;
}

auto ClientSession::SendBaseRpc(uint32_t /*entity_id*/, uint32_t rpc_id,
                                const uint8_t* payload, int32_t len) -> int32_t {
  if (state_ != ATLAS_NET_STATE_CONNECTED || !baseapp_channel_) {
    last_error_ = "AtlasNetSendBaseRpc requires Connected state";
    return ATLAS_NET_ERR_NOCONN;
  }
  ::atlas::baseapp::ClientBaseRpc msg;
  msg.rpc_id = rpc_id;
  if (len > 0 && payload) {
    msg.payload.assign(reinterpret_cast<const std::byte*>(payload),
                       reinterpret_cast<const std::byte*>(payload) + len);
  }
  (void)baseapp_channel_->SendMessage(msg);
  return ATLAS_NET_OK;
}

auto ClientSession::SendCellRpc(uint32_t entity_id, uint32_t rpc_id,
                                const uint8_t* payload, int32_t len) -> int32_t {
  if (state_ != ATLAS_NET_STATE_CONNECTED || !baseapp_channel_) {
    last_error_ = "AtlasNetSendCellRpc requires Connected state";
    return ATLAS_NET_ERR_NOCONN;
  }
  ::atlas::baseapp::ClientCellRpc msg;
  msg.target_entity_id = entity_id;
  msg.rpc_id = rpc_id;
  if (len > 0 && payload) {
    msg.payload.assign(reinterpret_cast<const std::byte*>(payload),
                       reinterpret_cast<const std::byte*>(payload) + len);
  }
  (void)baseapp_channel_->SendMessage(msg);
  return ATLAS_NET_OK;
}

auto ClientSession::SetCallbacks(const AtlasNetCallbacks& cb) -> int32_t {
  callbacks_ = cb;

  auto noop_disconnect      = [](AtlasNetContext*, int32_t) {};
  auto noop_player_base     = [](AtlasNetContext*, uint32_t, uint16_t,
                                 const uint8_t*, int32_t) {};
  auto noop_player_cell     = [](AtlasNetContext*, uint32_t,
                                 float, float, float, float, float, float,
                                 const uint8_t*, int32_t) {};
  auto noop_reset           = [](AtlasNetContext*) {};
  auto noop_entity_enter    = [](AtlasNetContext*, uint32_t, uint16_t,
                                 float, float, float, float, float, float,
                                 const uint8_t*, int32_t) {};
  auto noop_entity_leave    = [](AtlasNetContext*, uint32_t) {};
  auto noop_entity_position = [](AtlasNetContext*, uint32_t,
                                 float, float, float, float, float, float,
                                 uint8_t) {};
  auto noop_entity_property = [](AtlasNetContext*, uint32_t, uint8_t,
                                 const uint8_t*, int32_t) {};
  auto noop_forced_position = [](AtlasNetContext*, uint32_t,
                                 float, float, float, float, float, float) {};
  auto noop_rpc             = [](AtlasNetContext*, uint32_t, uint32_t,
                                 const uint8_t*, int32_t) {};

  if (!callbacks_.on_disconnect)         callbacks_.on_disconnect         = +noop_disconnect;
  if (!callbacks_.on_player_base_create) callbacks_.on_player_base_create = +noop_player_base;
  if (!callbacks_.on_player_cell_create) callbacks_.on_player_cell_create = +noop_player_cell;
  if (!callbacks_.on_reset_entities)     callbacks_.on_reset_entities     = +noop_reset;
  if (!callbacks_.on_entity_enter)       callbacks_.on_entity_enter       = +noop_entity_enter;
  if (!callbacks_.on_entity_leave)       callbacks_.on_entity_leave       = +noop_entity_leave;
  if (!callbacks_.on_entity_position)    callbacks_.on_entity_position    = +noop_entity_position;
  if (!callbacks_.on_entity_property)    callbacks_.on_entity_property    = +noop_entity_property;
  if (!callbacks_.on_forced_position)    callbacks_.on_forced_position    = +noop_forced_position;
  if (!callbacks_.on_rpc)                callbacks_.on_rpc                = +noop_rpc;
  return ATLAS_NET_OK;
}

auto ClientSession::FillStats(AtlasNetStats* out) const -> int32_t {
  if (!out) return ATLAS_NET_ERR_INVAL;
  std::memset(out, 0, sizeof(*out));
  return ATLAS_NET_OK;
}

void ClientSession::CloseLoginAppChannel() {
  if (loginapp_channel_) {
    loginapp_channel_->Condemn();
    loginapp_channel_ = nullptr;
  }
}

void ClientSession::CloseBaseAppChannel() {
  if (baseapp_channel_) {
    baseapp_channel_->Condemn();
    baseapp_channel_ = nullptr;
  }
}

void ClientSession::ClearSessionKey() {
  SecureZero(session_key_.data(), session_key_.size());
  baseapp_addr_ = Address{};
}

}  // namespace atlas::net_client
