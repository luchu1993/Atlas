#include "net_client/client_session.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "foundation/log.h"
#include "loginapp/login_messages.h"
#include "movement_sim/movement_sim.h"
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
    case LoginStatus::kSuccess:
      return ATLAS_LOGIN_SUCCESS;
    case LoginStatus::kInvalidCredentials:
      return ATLAS_LOGIN_INVALID_CREDENTIALS;
    case LoginStatus::kAlreadyLoggedIn:
      return ATLAS_LOGIN_ALREADY_LOGGED_IN;
    case LoginStatus::kServerFull:
      return ATLAS_LOGIN_SERVER_FULL;
    case LoginStatus::kRateLimited:
    case LoginStatus::kServerNotReady:
    case LoginStatus::kServerBusy:
      return ATLAS_LOGIN_SERVER_FULL;
    case LoginStatus::kDefMismatch:
      return ATLAS_LOGIN_DEF_MISMATCH;
    case LoginStatus::kInternalError:
    case LoginStatus::kLoginInProgress:
      return ATLAS_LOGIN_INTERNAL_ERROR;
  }
  return ATLAS_LOGIN_INTERNAL_ERROR;
}

auto FormatHostOnly(const Address& addr) -> std::string {
  const uint32_t ip = addr.Ip();
  const auto* bytes = reinterpret_cast<const uint8_t*>(&ip);
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

auto ClientSession::StartLogin(std::string_view loginapp_host, uint16_t loginapp_port,
                               std::string_view username, std::string_view password_hash,
                               AtlasLoginResultFn callback, void* user_data) -> int32_t {
  if (state_ != ATLAS_NET_STATE_DISCONNECTED) {
    last_error_ = "AtlasNetLogin requires Disconnected state";
    return ATLAS_NET_ERR_BUSY;
  }
  if (loginapp_host.empty() || loginapp_port == 0 || !callback) {
    last_error_ = "AtlasNetLogin: invalid args";
    return ATLAS_NET_ERR_INVAL;
  }

  Address login_addr(loginapp_host, loginapp_port);
  auto ch_result = network_.ConnectRudp(login_addr, NetworkInterface::InternetRudpProfile());
  if (!ch_result) {
    last_error_ = std::format("ConnectRudp(loginapp) failed: {}", ch_result.Error().Message());
    ATLAS_LOG_ERROR("ClientSession: {}", last_error_);
    return ATLAS_NET_ERR_NOCONN;
  }
  loginapp_channel_ = *ch_result;
  ApplyTransportImpairment(loginapp_channel_);

  auto reg = network_.InterfaceTable().RegisterTypedHandler<::atlas::login::LoginResult>(
      [this](const Address&, Channel*, const ::atlas::login::LoginResult& msg) {
        OnLoginResult(msg);
      });
  if (!reg) {
    last_error_ = std::format("Register LoginResult handler failed: {}", reg.Error().Message());
    CloseLoginAppChannel();
    return ATLAS_NET_ERR_INVAL;
  }

  ::atlas::login::LoginRequest req;
  req.username = std::string(username);
  req.password_hash = std::string(password_hash);
  req.entity_def_digest = entity_def_digest_;
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

void ClientSession::FireLoginCallback(AtlasLoginStatus status, std::string_view host, uint16_t port,
                                      std::string_view error) {
  auto cb = std::exchange(login_callback_, nullptr);
  void* ud = std::exchange(login_user_data_, nullptr);
  if (!cb) return;

  std::string host_buf{host};
  std::string err_buf{error};
  cb(ud, static_cast<uint8_t>(status), status == ATLAS_LOGIN_SUCCESS ? host_buf.c_str() : nullptr,
     status == ATLAS_LOGIN_SUCCESS ? port : static_cast<uint16_t>(0),
     err_buf.empty() ? nullptr : err_buf.c_str());
}

void ClientSession::CancelLoginTimeout() {
  if (login_timeout_.IsValid()) {
    dispatcher_.CancelTimer(login_timeout_);
    login_timeout_ = TimerHandle{};
  }
}

auto ClientSession::StartAuthenticate(AtlasAuthResultFn callback, void* user_data) -> int32_t {
  if (state_ != ATLAS_NET_STATE_LOGIN_SUCCEEDED) {
    last_error_ = "AtlasNetAuthenticate requires LoginSucceeded state";
    return ATLAS_NET_ERR_BUSY;
  }
  if (!callback) {
    last_error_ = "AtlasNetAuthenticate: callback is NULL";
    return ATLAS_NET_ERR_INVAL;
  }

  auto ch_result = network_.ConnectRudp(baseapp_addr_, NetworkInterface::InternetRudpProfile());
  if (!ch_result) {
    last_error_ = std::format("ConnectRudp(baseapp) failed: {}", ch_result.Error().Message());
    ATLAS_LOG_ERROR("ClientSession: {}", last_error_);
    ClearSessionKey();
    state_ = ATLAS_NET_STATE_DISCONNECTED;
    return ATLAS_NET_ERR_NOCONN;
  }
  baseapp_channel_ = *ch_result;
  ApplyTransportImpairment(baseapp_channel_);

  auto reg = network_.InterfaceTable().RegisterTypedHandler<::atlas::baseapp::AuthenticateResult>(
      [this](const Address&, Channel*, const ::atlas::baseapp::AuthenticateResult& msg) {
        OnAuthResult(msg);
      });
  if (!reg) {
    last_error_ =
        std::format("Register AuthenticateResult handler failed: {}", reg.Error().Message());
    CloseBaseAppChannel();
    ClearSessionKey();
    state_ = ATLAS_NET_STATE_DISCONNECTED;
    return ATLAS_NET_ERR_INVAL;
  }

  ::atlas::baseapp::Authenticate auth_msg;
  std::memcpy(auth_msg.session_key.bytes, session_key_.data(), session_key_.size());
  auto send_result = baseapp_channel_->SendMessage(auth_msg);
  ATLAS_LOG_INFO("ClientSession: sent Authenticate to {} (send_ok={})",
                 baseapp_addr_.ToString(), static_cast<bool>(send_result));

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
  ATLAS_LOG_INFO("ClientSession: received AuthenticateResult success={} entity_id={} type_id={}",
                 msg.success, msg.entity_id, msg.type_id);
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
  cb(ud, success ? uint8_t{1} : uint8_t{0}, eid, tid, err_buf.empty() ? nullptr : err_buf.c_str());
}

void ClientSession::CancelAuthTimeout() {
  if (auth_timeout_.IsValid()) {
    dispatcher_.CancelTimer(auth_timeout_);
    auth_timeout_ = TimerHandle{};
  }
}

void ClientSession::InstallDefaultHandler() {
  // Typed handlers for fixed-length BaseApp-to-client msgs; the default handler's
  // packed-int fallback would misparse the first body byte as a length prefix.
  (void)network_.InterfaceTable().RegisterTypedHandler<::atlas::baseapp::EntityTransferred>(
      [this](const Address&, Channel*, const ::atlas::baseapp::EntityTransferred& msg) {
        uint8_t body[sizeof(uint32_t) + sizeof(uint16_t)];
        std::memcpy(body, &msg.new_entity_id, sizeof(uint32_t));
        std::memcpy(body + sizeof(uint32_t), &msg.new_type_id, sizeof(uint16_t));
        callbacks_.on_deliver(
            reinterpret_cast<AtlasNetContext*>(this),
            static_cast<uint16_t>(msg_id::Id(msg_id::BaseApp::kEntityTransferred)), body,
            static_cast<int32_t>(sizeof(body)));
      });
  (void)network_.InterfaceTable().RegisterTypedHandler<::atlas::baseapp::CellReady>(
      [this](const Address&, Channel*, const ::atlas::baseapp::CellReady& msg) {
        uint8_t body[sizeof(uint32_t)];
        std::memcpy(body, &msg.entity_id, sizeof(uint32_t));
        callbacks_.on_deliver(reinterpret_cast<AtlasNetContext*>(this),
                              static_cast<uint16_t>(msg_id::Id(msg_id::BaseApp::kCellReady)), body,
                              static_cast<int32_t>(sizeof(body)));
      });
  (void)network_.InterfaceTable().RegisterTypedHandler<::atlas::baseapp::MovementStateAckToClient>(
      [this](const Address&, Channel*, const ::atlas::baseapp::MovementStateAckToClient& msg) {
        BinaryWriter writer(::atlas::baseapp::MovementStateAckToClient::Descriptor().fixed_length);
        msg.Serialize(writer);
        auto data = writer.Data();
        callbacks_.on_deliver(reinterpret_cast<AtlasNetContext*>(this),
                              ::atlas::baseapp::kClientMovementStateAckMessageId,
                              reinterpret_cast<const uint8_t*>(data.data()),
                              static_cast<int32_t>(data.size()));
      });
  (void)network_.InterfaceTable().RegisterTypedHandler<
      ::atlas::baseapp::MovementCommandStartToClient>(
      [this](const Address&, Channel*,
             const ::atlas::baseapp::MovementCommandStartToClient& msg) {
        BinaryWriter writer(
            ::atlas::baseapp::MovementCommandStartToClient::Descriptor().fixed_length);
        msg.Serialize(writer);
        auto data = writer.Data();
        callbacks_.on_deliver(reinterpret_cast<AtlasNetContext*>(this),
                              ::atlas::baseapp::kClientMovementCommandStartMessageId,
                              reinterpret_cast<const uint8_t*>(data.data()),
                              static_cast<int32_t>(data.size()));
      });
  (void)network_.InterfaceTable().RegisterTypedHandler<
      ::atlas::baseapp::MovementCommandEndToClient>(
      [this](const Address&, Channel*,
             const ::atlas::baseapp::MovementCommandEndToClient& msg) {
        BinaryWriter writer(
            ::atlas::baseapp::MovementCommandEndToClient::Descriptor().fixed_length);
        msg.Serialize(writer);
        auto data = writer.Data();
        callbacks_.on_deliver(reinterpret_cast<AtlasNetContext*>(this),
                              ::atlas::baseapp::kClientMovementCommandEndMessageId,
                              reinterpret_cast<const uint8_t*>(data.data()),
                              static_cast<int32_t>(data.size()));
      });

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
        callbacks_.on_deliver(reinterpret_cast<AtlasNetContext*>(this), static_cast<uint16_t>(id),
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
  // Same-id re-registration returns kAlreadyExists; clear so the next
  // StartLogin / OnAuthResult can re-install its typed handlers.
  network_.InterfaceTable().UnregisterAllHandlers();
  network_.InterfaceTable().SetDefaultHandler(nullptr);

  // Reuse-source-port reconnects hit a stale server-side channel before its
  // inactivity timeout kicks in; close the socket so we bind a fresh port.
  network_.CloseRudpSocket();

  state_ = ATLAS_NET_STATE_DISCONNECTED;
  player_entity_id_ = kInvalidEntityID;
  player_type_id_ = 0;

  if (reason == ATLAS_DISCONNECT_LOGOUT && was_connected) {
    callbacks_.on_disconnect(reinterpret_cast<AtlasNetContext*>(this), kRpcReason_LoggedOff);
  }
  return ATLAS_NET_OK;
}

auto ClientSession::SetTransportImpairment(uint32_t one_way_latency_ms, uint32_t loss_permyriad,
                                           uint32_t seed) -> int32_t {
  if (loss_permyriad > 10'000) {
    last_error_ = "AtlasNetSetTransportImpairment: loss_permyriad must be <= 10000";
    return ATLAS_NET_ERR_INVAL;
  }
  transport_impairment_latency_ms_ = one_way_latency_ms;
  transport_impairment_loss_permyriad_ = loss_permyriad;
  transport_impairment_seed_ = seed != 0 ? seed : 1u;
  ApplyTransportImpairment(loginapp_channel_);
  ApplyTransportImpairment(baseapp_channel_);
  return ATLAS_NET_OK;
}

auto ClientSession::SendBaseRpc(uint32_t /*entity_id*/, uint32_t rpc_id, const uint8_t* payload,
                                int32_t len) -> int32_t {
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

auto ClientSession::SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload,
                                int32_t len) -> int32_t {
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

auto ClientSession::SendMovementInput(uint32_t target_entity_id,
                                      const AtlasMovementInputFrame* frames,
                                      int32_t frame_count) -> int32_t {
  if (state_ != ATLAS_NET_STATE_CONNECTED || !baseapp_channel_) {
    last_error_ = "AtlasNetSendMovementInput requires Connected state";
    return ATLAS_NET_ERR_NOCONN;
  }
  if (target_entity_id == kInvalidEntityID || frames == nullptr || frame_count <= 0 ||
      frame_count > static_cast<int32_t>(movement::kMaxMovementInputFrames)) {
    last_error_ = "AtlasNetSendMovementInput: invalid args";
    return ATLAS_NET_ERR_INVAL;
  }

  ::atlas::baseapp::ClientMovementInput msg;
  msg.target_entity_id = target_entity_id;
  msg.frames.reserve(static_cast<std::size_t>(frame_count));
  for (int32_t i = 0; i < frame_count; ++i) {
    movement::InputFrame frame;
    frame.seq = frames[i].seq;
    frame.input_tick = frames[i].input_tick;
    frame.move_x = frames[i].move_x;
    frame.move_z = frames[i].move_z;
    frame.view_yaw = frames[i].view_yaw;
    frame.view_pitch = frames[i].view_pitch;
    frame.buttons = frames[i].buttons;
    frame.client_dt_ms = frames[i].client_dt_ms;
    if (!movement::IsInputFrameValid(frame)) {
      last_error_ = "AtlasNetSendMovementInput: invalid input frame";
      return ATLAS_NET_ERR_INVAL;
    }
    msg.frames.push_back(frame);
  }
  if (auto result = baseapp_channel_->SendMessage(msg); !result) {
    last_error_ = std::format("AtlasNetSendMovementInput failed: {}", result.Error().Message());
    return ATLAS_NET_ERR_NOCONN;
  }
  return ATLAS_NET_OK;
}

auto ClientSession::SendMovementCorrectionReport(uint32_t target_entity_id,
                                                 uint32_t acked_input_seq,
                                                 uint32_t server_tick,
                                                 float distance_m,
                                                 uint16_t correction_flags) -> int32_t {
  if (state_ != ATLAS_NET_STATE_CONNECTED || !baseapp_channel_) {
    last_error_ = "AtlasNetSendMovementCorrectionReport requires Connected state";
    return ATLAS_NET_ERR_NOCONN;
  }
  const uint16_t valid_flags = movement::kCorrectionFlagTier1 |
      movement::kCorrectionFlagTier2 | movement::kCorrectionFlagSnap;
  if (target_entity_id == kInvalidEntityID || !std::isfinite(distance_m) ||
      distance_m < 0.0f || (correction_flags & ~valid_flags) != 0) {
    last_error_ = "AtlasNetSendMovementCorrectionReport: invalid args";
    return ATLAS_NET_ERR_INVAL;
  }

  ::atlas::baseapp::MovementCorrectionReport msg;
  msg.target_entity_id = target_entity_id;
  msg.acked_input_seq = acked_input_seq;
  msg.server_tick = server_tick;
  msg.distance_m = distance_m;
  msg.correction_flags = correction_flags;
  if (auto result = baseapp_channel_->SendMessage(msg); !result) {
    last_error_ =
        std::format("AtlasNetSendMovementCorrectionReport failed: {}", result.Error().Message());
    return ATLAS_NET_ERR_NOCONN;
  }
  return ATLAS_NET_OK;
}

auto ClientSession::SetCallbacks(const AtlasNetCallbacks& cb) -> int32_t {
  callbacks_ = cb;

  auto noop_disconnect = [](AtlasNetContext*, int32_t) {};
  auto noop_deliver = [](AtlasNetContext*, uint16_t, const uint8_t*, int32_t) {};

  if (!callbacks_.on_disconnect) callbacks_.on_disconnect = noop_disconnect;
  if (!callbacks_.on_deliver) callbacks_.on_deliver = noop_deliver;
  return ATLAS_NET_OK;
}

auto ClientSession::FillStats(AtlasNetStats* out) const -> int32_t {
  if (!out) return ATLAS_NET_ERR_INVAL;
  std::memset(out, 0, sizeof(*out));
  // Only read channel state while live; after a long Editor pause, the
  // dispatcher may reap timed-out RUDP before stats code sees the pointer.
  if (state_ != ATLAS_NET_STATE_CONNECTED && state_ != ATLAS_NET_STATE_AUTHENTICATING &&
      state_ != ATLAS_NET_STATE_LOGGING_IN && state_ != ATLAS_NET_STATE_LOGIN_SUCCEEDED) {
    return ATLAS_NET_OK;
  }
  Channel* ch = baseapp_channel_ ? baseapp_channel_ : loginapp_channel_;
  if (ch && !ch->IsCondemned()) {
    // uint32 truncation is intentional: ABI pins these fields and MVP sessions
    // stay well under 4 GiB per direction.
    out->bytes_sent = static_cast<uint32_t>(ch->BytesSent());
    out->bytes_recv = static_cast<uint32_t>(ch->BytesReceived());
    if (auto* rudp = dynamic_cast<ReliableUdpChannel*>(ch)) {
      out->rtt_ms = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(rudp->Rtt()).count());
      out->send_queue_size = rudp->UnackedCount();
    }
  }
  return ATLAS_NET_OK;
}

void ClientSession::SetEntityDefDigest(const uint8_t* data, int32_t len) {
  if (!data || len != static_cast<int32_t>(entity_def_digest_.size())) return;
  std::memcpy(entity_def_digest_.data(), data, entity_def_digest_.size());
}

void ClientSession::CloseLoginAppChannel() {
  if (loginapp_channel_) {
    // CondemnChannel removes the entry from NetworkInterface.channels_; without
    // it a subsequent ConnectRudp(login_addr) would reuse this dead channel.
    network_.DisconnectChannel(loginapp_channel_->RemoteAddress());
    loginapp_channel_ = nullptr;
  }
}

void ClientSession::CloseBaseAppChannel() {
  if (baseapp_channel_) {
    network_.DisconnectChannel(baseapp_channel_->RemoteAddress());
    baseapp_channel_ = nullptr;
  }
}

void ClientSession::ClearSessionKey() {
  SecureZero(session_key_.data(), session_key_.size());
  baseapp_addr_ = Address{};
}

void ClientSession::ApplyTransportImpairment(Channel* channel) const {
  if (auto* rudp = dynamic_cast<ReliableUdpChannel*>(channel)) {
    rudp->SetTransportImpairment(transport_impairment_latency_ms_,
                                 transport_impairment_loss_permyriad_,
                                 transport_impairment_seed_);
  }
}

}  // namespace atlas::net_client
