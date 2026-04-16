#ifndef ATLAS_SERVER_LOGINAPP_LOGIN_MESSAGES_H_
#define ATLAS_SERVER_LOGINAPP_LOGIN_MESSAGES_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "db/idatabase.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "server/entity_types.h"

// ============================================================================
// Login-flow network messages (IDs 5000–5007)
//
// Directions:
//   Client     → LoginApp  : LoginRequest       (5000)
//   LoginApp   → Client    : LoginResult        (5001)
//   LoginApp   → DBApp     : AuthLogin          (5002)
//   DBApp      → LoginApp  : AuthLoginResult    (5003)
//   LoginApp   → BaseAppMgr: AllocateBaseApp    (5004)
//   BaseAppMgr → LoginApp  : AllocateBaseAppResult (5005)
//   LoginApp   → BaseApp   : PrepareLogin       (5006)
//   BaseApp    → LoginApp  : PrepareLoginResult (5007)
//   LoginApp   → BaseApp   : CancelPrepareLogin (5008)
// ============================================================================

namespace atlas::login {

// ============================================================================
// LoginStatus
// ============================================================================

enum class LoginStatus : uint8_t {
  kSuccess = 0,
  kInvalidCredentials = 1,
  kAlreadyLoggedIn = 2,
  kServerFull = 3,
  kRateLimited = 4,
  kServerNotReady = 5,
  kInternalError = 6,
  kLoginInProgress = 7,
  kServerBusy = 8,
};

// ============================================================================
// LoginRequest  (Client → LoginApp, ID 5000)
// ============================================================================

struct LoginRequest {
  std::string username;
  std::string password_hash;  // SHA-256 hex of password

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kLoginRequest), "login::LoginRequest",
                                   MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WriteString(username);
    w.WriteString(password_hash);
  }

  static auto Deserialize(BinaryReader& r) -> Result<LoginRequest> {
    auto u = r.ReadString();
    auto p = r.ReadString();
    if (!u || !p) return Error{ErrorCode::kInvalidArgument, "LoginRequest: truncated"};
    LoginRequest msg;
    msg.username = std::move(*u);
    msg.password_hash = std::move(*p);
    return msg;
  }
};
static_assert(NetworkMessage<LoginRequest>);

// ============================================================================
// LoginResult  (LoginApp → Client, ID 5001)
// ============================================================================

struct LoginResult {
  LoginStatus status{LoginStatus::kInternalError};
  SessionKey session_key;
  Address baseapp_addr;
  std::string error_message;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kLoginResult), "login::LoginResult",
                                   MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(static_cast<uint8_t>(status));
    if (status == LoginStatus::kSuccess) {
      w.WriteBytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(session_key.bytes),
                                              sizeof(session_key.bytes)));
      w.Write(baseapp_addr.Ip());
      w.Write(baseapp_addr.Port());
    } else {
      w.WriteString(error_message);
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<LoginResult> {
    auto st = r.Read<uint8_t>();
    if (!st) return Error{ErrorCode::kInvalidArgument, "LoginResult: truncated"};
    LoginResult msg;
    msg.status = static_cast<LoginStatus>(*st);
    if (msg.status == LoginStatus::kSuccess) {
      auto key_span = r.ReadBytes(sizeof(SessionKey));
      auto ip = r.Read<uint32_t>();
      auto port = r.Read<uint16_t>();
      if (!key_span || !ip || !port)
        return Error{ErrorCode::kInvalidArgument, "LoginResult: field truncated"};
      std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
      msg.baseapp_addr = Address(*ip, *port);
    } else {
      auto err = r.ReadString();
      if (!err) return Error{ErrorCode::kInvalidArgument, "LoginResult: error_message truncated"};
      msg.error_message = std::move(*err);
    }
    return msg;
  }
};
static_assert(NetworkMessage<LoginResult>);

// ============================================================================
// AuthLogin  (LoginApp → DBApp, ID 5002)
// ============================================================================

struct AuthLogin {
  uint32_t request_id{0};
  std::string username;
  std::string password_hash;
  bool auto_create{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kAuthLogin), "login::AuthLogin",
                                   MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.WriteString(username);
    w.WriteString(password_hash);
    w.Write(static_cast<uint8_t>(auto_create ? 1 : 0));
  }

  static auto Deserialize(BinaryReader& r) -> Result<AuthLogin> {
    auto rid = r.Read<uint32_t>();
    auto u = r.ReadString();
    auto p = r.ReadString();
    auto ac = r.Read<uint8_t>();
    if (!rid || !u || !p || !ac) return Error{ErrorCode::kInvalidArgument, "AuthLogin: truncated"};
    AuthLogin msg;
    msg.request_id = *rid;
    msg.username = std::move(*u);
    msg.password_hash = std::move(*p);
    msg.auto_create = (*ac != 0);
    return msg;
  }
};
static_assert(NetworkMessage<AuthLogin>);

// ============================================================================
// AuthLoginResult  (DBApp → LoginApp, ID 5003)
// ============================================================================

struct AuthLoginResult {
  uint32_t request_id{0};
  bool success{false};
  LoginStatus status{LoginStatus::kInternalError};
  DatabaseID dbid{kInvalidDBID};
  uint16_t type_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::Login::kAuthLoginResult), "login::AuthLoginResult",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(int64_t) +
                         sizeof(uint16_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(static_cast<uint8_t>(status));
    w.Write(dbid);
    w.Write(type_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<AuthLoginResult> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto st = r.Read<uint8_t>();
    auto db = r.Read<int64_t>();
    auto ti = r.Read<uint16_t>();
    if (!rid || !ok || !st || !db || !ti)
      return Error{ErrorCode::kInvalidArgument, "AuthLoginResult: truncated"};
    AuthLoginResult msg;
    msg.request_id = *rid;
    msg.success = (*ok != 0);
    msg.status = static_cast<LoginStatus>(*st);
    msg.dbid = *db;
    msg.type_id = *ti;
    return msg;
  }
};
static_assert(NetworkMessage<AuthLoginResult>);

// ============================================================================
// AllocateBaseApp  (LoginApp → BaseAppMgr, ID 5004)
// ============================================================================

struct AllocateBaseApp {
  uint32_t request_id{0};
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::Login::kAllocateBaseApp), "login::AllocateBaseApp",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t) + sizeof(int64_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(type_id);
    w.Write(dbid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<AllocateBaseApp> {
    auto rid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    if (!rid || !ti || !db) return Error{ErrorCode::kInvalidArgument, "AllocateBaseApp: truncated"};
    AllocateBaseApp msg;
    msg.request_id = *rid;
    msg.type_id = *ti;
    msg.dbid = *db;
    return msg;
  }
};
static_assert(NetworkMessage<AllocateBaseApp>);

// ============================================================================
// AllocateBaseAppResult  (BaseAppMgr → LoginApp, ID 5005)
// ============================================================================

struct AllocateBaseAppResult {
  uint32_t request_id{0};
  bool success{false};
  Address internal_addr;  // BaseApp 内部地址 (LoginApp → BaseApp 发 PrepareLogin)
  Address external_addr;  // BaseApp 外部地址 (返回给客户端)

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kAllocateBaseAppResult),
                                   "login::AllocateBaseAppResult", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t) +
                                                    (sizeof(uint32_t) + sizeof(uint16_t)) * 2)};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(internal_addr.Ip());
    w.Write(internal_addr.Port());
    w.Write(external_addr.Ip());
    w.Write(external_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<AllocateBaseAppResult> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto iip = r.Read<uint32_t>();
    auto iport = r.Read<uint16_t>();
    auto eip = r.Read<uint32_t>();
    auto eport = r.Read<uint16_t>();
    if (!rid || !ok || !iip || !iport || !eip || !eport)
      return Error{ErrorCode::kInvalidArgument, "AllocateBaseAppResult: truncated"};
    AllocateBaseAppResult msg;
    msg.request_id = *rid;
    msg.success = (*ok != 0);
    msg.internal_addr = Address(*iip, *iport);
    msg.external_addr = Address(*eip, *eport);
    return msg;
  }
};
static_assert(NetworkMessage<AllocateBaseAppResult>);

// ============================================================================
// PrepareLogin  (LoginApp → BaseApp, ID 5006)
// ============================================================================

struct PrepareLogin {
  static constexpr uint32_t kMaxBlobSize = 1024 * 1024;  // 1 MB

  uint32_t request_id{0};
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};
  SessionKey session_key;
  Address client_addr;
  std::vector<std::byte> entity_blob;
  bool blob_prefetched{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kPrepareLogin), "login::PrepareLogin",
                                   MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(type_id);
    w.Write(dbid);
    w.WriteBytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(session_key.bytes),
                                            sizeof(session_key.bytes)));
    w.Write(client_addr.Ip());
    w.Write(client_addr.Port());
    w.Write(static_cast<uint8_t>(blob_prefetched ? 1 : 0));
    w.Write(static_cast<uint32_t>(entity_blob.size()));
    if (!entity_blob.empty()) w.WriteBytes(entity_blob);
  }

  static auto Deserialize(BinaryReader& r) -> Result<PrepareLogin> {
    auto rid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    if (!rid || !ti || !db) return Error{ErrorCode::kInvalidArgument, "PrepareLogin: truncated"};
    auto key_span = r.ReadBytes(sizeof(SessionKey));
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto pf = r.Read<uint8_t>();
    auto blob_sz = r.Read<uint32_t>();
    if (!key_span || !ip || !port || !pf || !blob_sz)
      return Error{ErrorCode::kInvalidArgument, "PrepareLogin: field truncated"};
    PrepareLogin msg;
    msg.request_id = *rid;
    msg.type_id = *ti;
    msg.dbid = *db;
    std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
    msg.client_addr = Address(*ip, *port);
    msg.blob_prefetched = (*pf != 0);
    if (*blob_sz > kMaxBlobSize)
      return Error{ErrorCode::kInvalidArgument, "PrepareLogin: blob too large"};
    if (*blob_sz > 0) {
      auto blob_span = r.ReadBytes(*blob_sz);
      if (!blob_span) return Error{ErrorCode::kInvalidArgument, "PrepareLogin: blob truncated"};
      msg.entity_blob.assign(blob_span->begin(), blob_span->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<PrepareLogin>);

// ============================================================================
// PrepareLoginResult  (BaseApp → LoginApp, ID 5007)
// ============================================================================

struct PrepareLoginResult {
  uint32_t request_id{0};
  bool success{false};
  EntityID entity_id{kInvalidEntityID};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kPrepareLoginResult),
                                   "login::PrepareLoginResult", MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(entity_id);
    w.WriteString(error);
  }

  static auto Deserialize(BinaryReader& r) -> Result<PrepareLoginResult> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto eid = r.Read<uint32_t>();
    auto err = r.ReadString();
    if (!rid || !ok || !eid || !err)
      return Error{ErrorCode::kInvalidArgument, "PrepareLoginResult: truncated"};
    PrepareLoginResult msg;
    msg.request_id = *rid;
    msg.success = (*ok != 0);
    msg.entity_id = *eid;
    msg.error = std::move(*err);
    return msg;
  }
};
static_assert(NetworkMessage<PrepareLoginResult>);

// ============================================================================
// CancelPrepareLogin  (LoginApp → BaseApp, ID 5008)
// Sent when the client disconnects before the login handoff completes so the
// target BaseApp can roll back any pending prepare / checkout work.
// ============================================================================

struct CancelPrepareLogin {
  uint32_t request_id{0};
  DatabaseID dbid{kInvalidDBID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kCancelPrepareLogin),
                                   "login::CancelPrepareLogin", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(int64_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(dbid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CancelPrepareLogin> {
    auto rid = r.Read<uint32_t>();
    auto db = r.Read<int64_t>();
    if (!rid || !db) return Error{ErrorCode::kInvalidArgument, "CancelPrepareLogin: truncated"};
    CancelPrepareLogin msg;
    msg.request_id = *rid;
    msg.dbid = *db;
    return msg;
  }
};
static_assert(NetworkMessage<CancelPrepareLogin>);

}  // namespace atlas::login

#endif  // ATLAS_SERVER_LOGINAPP_LOGIN_MESSAGES_H_
