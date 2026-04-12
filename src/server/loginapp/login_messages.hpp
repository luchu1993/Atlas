#pragma once

#include "db/idatabase.hpp"
#include "network/address.hpp"
#include "network/message.hpp"
#include "network/message_ids.hpp"
#include "server/entity_types.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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
// ============================================================================

namespace atlas::login
{

// ============================================================================
// LoginStatus
// ============================================================================

enum class LoginStatus : uint8_t
{
    Success = 0,
    InvalidCredentials = 1,
    AlreadyLoggedIn = 2,
    ServerFull = 3,
    RateLimited = 4,
    ServerNotReady = 5,
    InternalError = 6,
    LoginInProgress = 7,
    ServerBusy = 8,
};

// ============================================================================
// LoginRequest  (Client → LoginApp, ID 5000)
// ============================================================================

struct LoginRequest
{
    std::string username;
    std::string password_hash;  // SHA-256 hex of password

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::Login::LoginRequest),
                                      "login::LoginRequest", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_string(username);
        w.write_string(password_hash);
    }

    static auto deserialize(BinaryReader& r) -> Result<LoginRequest>
    {
        auto u = r.read_string();
        auto p = r.read_string();
        if (!u || !p)
            return Error{ErrorCode::InvalidArgument, "LoginRequest: truncated"};
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

struct LoginResult
{
    LoginStatus status{LoginStatus::InternalError};
    SessionKey session_key;
    Address baseapp_addr;
    std::string error_message;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::Login::LoginResult), "login::LoginResult",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(static_cast<uint8_t>(status));
        w.write_bytes(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(session_key.bytes), sizeof(session_key.bytes)));
        w.write(baseapp_addr.ip());
        w.write(baseapp_addr.port());
        w.write_string(error_message);
    }

    static auto deserialize(BinaryReader& r) -> Result<LoginResult>
    {
        auto st = r.read<uint8_t>();
        if (!st)
            return Error{ErrorCode::InvalidArgument, "LoginResult: truncated"};
        auto key_span = r.read_bytes(sizeof(SessionKey));
        auto ip = r.read<uint32_t>();
        auto port = r.read<uint16_t>();
        auto err = r.read_string();
        if (!key_span || !ip || !port || !err)
            return Error{ErrorCode::InvalidArgument, "LoginResult: field truncated"};
        LoginResult msg;
        msg.status = static_cast<LoginStatus>(*st);
        std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
        msg.baseapp_addr = Address(*ip, *port);
        msg.error_message = std::move(*err);
        return msg;
    }
};
static_assert(NetworkMessage<LoginResult>);

// ============================================================================
// AuthLogin  (LoginApp → DBApp, ID 5002)
// ============================================================================

struct AuthLogin
{
    uint32_t request_id{0};
    std::string username;
    std::string password_hash;
    bool auto_create{false};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::Login::AuthLogin), "login::AuthLogin",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write_string(username);
        w.write_string(password_hash);
        w.write(static_cast<uint8_t>(auto_create ? 1 : 0));
    }

    static auto deserialize(BinaryReader& r) -> Result<AuthLogin>
    {
        auto rid = r.read<uint32_t>();
        auto u = r.read_string();
        auto p = r.read_string();
        auto ac = r.read<uint8_t>();
        if (!rid || !u || !p || !ac)
            return Error{ErrorCode::InvalidArgument, "AuthLogin: truncated"};
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

struct AuthLoginResult
{
    uint32_t request_id{0};
    bool success{false};
    LoginStatus status{LoginStatus::InternalError};
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::Login::AuthLoginResult),
                                      "login::AuthLoginResult", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(success ? 1 : 0));
        w.write(static_cast<uint8_t>(status));
        w.write(dbid);
        w.write(type_id);
    }

    static auto deserialize(BinaryReader& r) -> Result<AuthLoginResult>
    {
        auto rid = r.read<uint32_t>();
        auto ok = r.read<uint8_t>();
        auto st = r.read<uint8_t>();
        auto db = r.read<int64_t>();
        auto ti = r.read<uint16_t>();
        if (!rid || !ok || !st || !db || !ti)
            return Error{ErrorCode::InvalidArgument, "AuthLoginResult: truncated"};
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

struct AllocateBaseApp
{
    uint32_t request_id{0};
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{
            msg_id::id(msg_id::Login::AllocateBaseApp), "login::AllocateBaseApp",
            MessageLengthStyle::Fixed,
            static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t) + sizeof(int64_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(type_id);
        w.write(dbid);
    }

    static auto deserialize(BinaryReader& r) -> Result<AllocateBaseApp>
    {
        auto rid = r.read<uint32_t>();
        auto ti = r.read<uint16_t>();
        auto db = r.read<int64_t>();
        if (!rid || !ti || !db)
            return Error{ErrorCode::InvalidArgument, "AllocateBaseApp: truncated"};
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

struct AllocateBaseAppResult
{
    uint32_t request_id{0};
    bool success{false};
    Address internal_addr;  // BaseApp 内部地址 (LoginApp → BaseApp 发 PrepareLogin)
    Address external_addr;  // BaseApp 外部地址 (返回给客户端)

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::Login::AllocateBaseAppResult),
                                      "login::AllocateBaseAppResult", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t) +
                                                       (sizeof(uint32_t) + sizeof(uint16_t)) * 2)};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(success ? 1 : 0));
        w.write(internal_addr.ip());
        w.write(internal_addr.port());
        w.write(external_addr.ip());
        w.write(external_addr.port());
    }

    static auto deserialize(BinaryReader& r) -> Result<AllocateBaseAppResult>
    {
        auto rid = r.read<uint32_t>();
        auto ok = r.read<uint8_t>();
        auto iip = r.read<uint32_t>();
        auto iport = r.read<uint16_t>();
        auto eip = r.read<uint32_t>();
        auto eport = r.read<uint16_t>();
        if (!rid || !ok || !iip || !iport || !eip || !eport)
            return Error{ErrorCode::InvalidArgument, "AllocateBaseAppResult: truncated"};
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

struct PrepareLogin
{
    uint32_t request_id{0};
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};
    SessionKey session_key;
    Address client_addr;
    std::vector<std::byte> entity_blob;
    bool blob_prefetched{false};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::Login::PrepareLogin),
                                      "login::PrepareLogin", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(type_id);
        w.write(dbid);
        w.write_bytes(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(session_key.bytes), sizeof(session_key.bytes)));
        w.write(client_addr.ip());
        w.write(client_addr.port());
        w.write(static_cast<uint8_t>(blob_prefetched ? 1 : 0));
        w.write(static_cast<uint32_t>(entity_blob.size()));
        if (!entity_blob.empty())
            w.write_bytes(entity_blob);
    }

    static auto deserialize(BinaryReader& r) -> Result<PrepareLogin>
    {
        auto rid = r.read<uint32_t>();
        auto ti = r.read<uint16_t>();
        auto db = r.read<int64_t>();
        if (!rid || !ti || !db)
            return Error{ErrorCode::InvalidArgument, "PrepareLogin: truncated"};
        auto key_span = r.read_bytes(sizeof(SessionKey));
        auto ip = r.read<uint32_t>();
        auto port = r.read<uint16_t>();
        auto pf = r.read<uint8_t>();
        auto blob_sz = r.read<uint32_t>();
        if (!key_span || !ip || !port || !pf || !blob_sz)
            return Error{ErrorCode::InvalidArgument, "PrepareLogin: field truncated"};
        PrepareLogin msg;
        msg.request_id = *rid;
        msg.type_id = *ti;
        msg.dbid = *db;
        std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
        msg.client_addr = Address(*ip, *port);
        msg.blob_prefetched = (*pf != 0);
        if (*blob_sz > 0)
        {
            auto blob_span = r.read_bytes(*blob_sz);
            if (!blob_span)
                return Error{ErrorCode::InvalidArgument, "PrepareLogin: blob truncated"};
            msg.entity_blob.assign(blob_span->begin(), blob_span->end());
        }
        return msg;
    }
};
static_assert(NetworkMessage<PrepareLogin>);

// ============================================================================
// PrepareLoginResult  (BaseApp → LoginApp, ID 5007)
// ============================================================================

struct PrepareLoginResult
{
    uint32_t request_id{0};
    bool success{false};
    EntityID entity_id{kInvalidEntityID};
    std::string error;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::Login::PrepareLoginResult),
                                      "login::PrepareLoginResult", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(success ? 1 : 0));
        w.write(entity_id);
        w.write_string(error);
    }

    static auto deserialize(BinaryReader& r) -> Result<PrepareLoginResult>
    {
        auto rid = r.read<uint32_t>();
        auto ok = r.read<uint8_t>();
        auto eid = r.read<uint32_t>();
        auto err = r.read_string();
        if (!rid || !ok || !eid || !err)
            return Error{ErrorCode::InvalidArgument, "PrepareLoginResult: truncated"};
        PrepareLoginResult msg;
        msg.request_id = *rid;
        msg.success = (*ok != 0);
        msg.entity_id = *eid;
        msg.error = std::move(*err);
        return msg;
    }
};
static_assert(NetworkMessage<PrepareLoginResult>);

}  // namespace atlas::login
