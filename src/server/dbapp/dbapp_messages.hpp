#pragma once

#include "db/idatabase.hpp"
#include "network/address.hpp"
#include "network/message.hpp"
#include "network/message_ids.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// DBApp network messages (IDs 4000–4008)
//
// Direction:
//   BaseApp → DBApp : WriteEntity, CheckoutEntity, CheckinEntity,
//                     DeleteEntity, LookupEntity, AbortCheckout
//   DBApp → BaseApp : WriteEntityAck, CheckoutEntityAck, DeleteEntityAck,
//                     LookupEntityAck, AbortCheckoutAck
// ============================================================================

namespace atlas::dbapp
{

// ============================================================================
// LoadMode — how to identify the entity to checkout
// ============================================================================

enum class LoadMode : uint8_t
{
    ByDBID = 0,
    ByName = 1,
};

// ============================================================================
// CheckoutStatus — result code for CheckoutEntityAck
// ============================================================================

enum class CheckoutStatus : uint8_t
{
    Success = 0,
    NotFound = 1,
    AlreadyCheckedOut = 2,
    DbError = 3,
};

// ============================================================================
// WriteEntity  (BaseApp → DBApp, ID 4000)
// ============================================================================

struct WriteEntity
{
    WriteFlags flags{WriteFlags::None};
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};
    uint32_t entity_id{0};
    uint32_t request_id{0};
    std::string identifier;
    std::vector<std::byte> blob;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::WriteEntity), "dbapp::WriteEntity",
                                      MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(static_cast<uint8_t>(flags));
        w.write(type_id);
        w.write(dbid);
        w.write(entity_id);
        w.write(request_id);
        w.write_string(identifier);
        w.write(static_cast<uint32_t>(blob.size()));
        w.write_bytes(blob);
    }

    static auto deserialize(BinaryReader& r) -> Result<WriteEntity>
    {
        auto f = r.read<uint8_t>();
        auto ti = r.read<uint16_t>();
        auto db = r.read<int64_t>();
        auto eid = r.read<uint32_t>();
        auto rid = r.read<uint32_t>();
        auto id_str = r.read_string();
        auto blob_sz = r.read<uint32_t>();
        if (!f || !ti || !db || !eid || !rid || !id_str || !blob_sz)
            return Error{ErrorCode::InvalidArgument, "WriteEntity: truncated"};
        auto blob_span = r.read_bytes(*blob_sz);
        if (!blob_span)
            return Error{ErrorCode::InvalidArgument, "WriteEntity: blob truncated"};
        WriteEntity msg;
        msg.flags = static_cast<WriteFlags>(*f);
        msg.type_id = *ti;
        msg.dbid = *db;
        msg.entity_id = *eid;
        msg.request_id = *rid;
        msg.identifier = std::move(*id_str);
        msg.blob.assign(blob_span->begin(), blob_span->end());
        return msg;
    }
};
static_assert(NetworkMessage<WriteEntity>);

// ============================================================================
// WriteEntityAck  (DBApp → BaseApp, ID 4001)
// ============================================================================

struct WriteEntityAck
{
    uint32_t request_id{0};
    bool success{false};
    DatabaseID dbid{kInvalidDBID};
    std::string error;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::WriteEntityAck),
                                      "dbapp::WriteEntityAck", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(success ? 1 : 0));
        w.write(dbid);
        w.write_string(error);
    }

    static auto deserialize(BinaryReader& r) -> Result<WriteEntityAck>
    {
        auto rid = r.read<uint32_t>();
        auto ok = r.read<uint8_t>();
        auto db = r.read<int64_t>();
        auto err = r.read_string();
        if (!rid || !ok || !db || !err)
            return Error{ErrorCode::InvalidArgument, "WriteEntityAck: truncated"};
        WriteEntityAck msg;
        msg.request_id = *rid;
        msg.success = *ok != 0;
        msg.dbid = *db;
        msg.error = std::move(*err);
        return msg;
    }
};
static_assert(NetworkMessage<WriteEntityAck>);

// ============================================================================
// CheckoutEntity  (BaseApp → DBApp, ID 4002)
// ============================================================================

struct CheckoutEntity
{
    LoadMode mode{LoadMode::ByDBID};
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};
    std::string identifier;
    uint32_t entity_id{0};
    uint32_t request_id{0};
    Address owner_addr;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::CheckoutEntity),
                                      "dbapp::CheckoutEntity", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(static_cast<uint8_t>(mode));
        w.write(type_id);
        w.write(dbid);
        w.write_string(identifier);
        w.write(entity_id);
        w.write(request_id);
        w.write(owner_addr.ip());
        w.write(owner_addr.port());
    }

    static auto deserialize(BinaryReader& r) -> Result<CheckoutEntity>
    {
        auto m = r.read<uint8_t>();
        auto ti = r.read<uint16_t>();
        auto db = r.read<int64_t>();
        auto id_str = r.read_string();
        auto eid = r.read<uint32_t>();
        auto rid = r.read<uint32_t>();
        auto oip = r.read<uint32_t>();
        auto oport = r.read<uint16_t>();
        if (!m || !ti || !db || !id_str || !eid || !rid || !oip || !oport)
            return Error{ErrorCode::InvalidArgument, "CheckoutEntity: truncated"};
        CheckoutEntity msg;
        msg.mode = static_cast<LoadMode>(*m);
        msg.type_id = *ti;
        msg.dbid = *db;
        msg.identifier = std::move(*id_str);
        msg.entity_id = *eid;
        msg.request_id = *rid;
        msg.owner_addr = Address(*oip, *oport);
        return msg;
    }
};
static_assert(NetworkMessage<CheckoutEntity>);

// ============================================================================
// CheckoutEntityAck  (DBApp → BaseApp, ID 4003)
// ============================================================================

struct CheckoutEntityAck
{
    uint32_t request_id{0};
    CheckoutStatus status{CheckoutStatus::Success};
    DatabaseID dbid{kInvalidDBID};
    std::vector<std::byte> blob;
    Address holder_addr;
    uint32_t holder_app_id{0};
    uint32_t holder_entity_id{0};
    std::string error;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::CheckoutEntityAck),
                                      "dbapp::CheckoutEntityAck", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(status));
        w.write(dbid);
        w.write(static_cast<uint32_t>(blob.size()));
        w.write_bytes(blob);
        w.write(holder_addr.ip());
        w.write(holder_addr.port());
        w.write(holder_app_id);
        w.write(holder_entity_id);
        w.write_string(error);
    }

    static auto deserialize(BinaryReader& r) -> Result<CheckoutEntityAck>
    {
        auto rid = r.read<uint32_t>();
        auto st = r.read<uint8_t>();
        auto db = r.read<int64_t>();
        auto blob_sz = r.read<uint32_t>();
        if (!rid || !st || !db || !blob_sz)
            return Error{ErrorCode::InvalidArgument, "CheckoutEntityAck: truncated"};
        auto blob_span = r.read_bytes(*blob_sz);
        auto h_ip = r.read<uint32_t>();
        auto h_port = r.read<uint16_t>();
        auto h_app = r.read<uint32_t>();
        auto h_eid = r.read<uint32_t>();
        auto err = r.read_string();
        if (!blob_span || !h_ip || !h_port || !h_app || !h_eid || !err)
            return Error{ErrorCode::InvalidArgument, "CheckoutEntityAck: field truncated"};
        CheckoutEntityAck msg;
        msg.request_id = *rid;
        msg.status = static_cast<CheckoutStatus>(*st);
        msg.dbid = *db;
        msg.blob.assign(blob_span->begin(), blob_span->end());
        msg.holder_addr = Address(*h_ip, *h_port);
        msg.holder_app_id = *h_app;
        msg.holder_entity_id = *h_eid;
        msg.error = std::move(*err);
        return msg;
    }
};
static_assert(NetworkMessage<CheckoutEntityAck>);

// ============================================================================
// CheckinEntity  (BaseApp → DBApp, ID 4004)
// ============================================================================

struct CheckinEntity
{
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::CheckinEntity),
                                      "dbapp::CheckinEntity", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint16_t) + sizeof(int64_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(type_id);
        w.write(dbid);
    }

    static auto deserialize(BinaryReader& r) -> Result<CheckinEntity>
    {
        auto ti = r.read<uint16_t>();
        auto db = r.read<int64_t>();
        if (!ti || !db)
            return Error{ErrorCode::InvalidArgument, "CheckinEntity: truncated"};
        CheckinEntity msg;
        msg.type_id = *ti;
        msg.dbid = *db;
        return msg;
    }
};
static_assert(NetworkMessage<CheckinEntity>);

// ============================================================================
// DeleteEntity  (BaseApp → DBApp, ID 4005)
// ============================================================================

struct DeleteEntity
{
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};
    uint32_t request_id{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{
            msg_id::id(msg_id::DBApp::DeleteEntity), "dbapp::DeleteEntity",
            MessageLengthStyle::Fixed,
            static_cast<int>(sizeof(uint16_t) + sizeof(int64_t) + sizeof(uint32_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(type_id);
        w.write(dbid);
        w.write(request_id);
    }

    static auto deserialize(BinaryReader& r) -> Result<DeleteEntity>
    {
        auto ti = r.read<uint16_t>();
        auto db = r.read<int64_t>();
        auto rid = r.read<uint32_t>();
        if (!ti || !db || !rid)
            return Error{ErrorCode::InvalidArgument, "DeleteEntity: truncated"};
        DeleteEntity msg;
        msg.type_id = *ti;
        msg.dbid = *db;
        msg.request_id = *rid;
        return msg;
    }
};
static_assert(NetworkMessage<DeleteEntity>);

// ============================================================================
// DeleteEntityAck  (DBApp → BaseApp, ID 4006)
// ============================================================================

struct DeleteEntityAck
{
    uint32_t request_id{0};
    bool success{false};
    std::string error;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::DeleteEntityAck),
                                      "dbapp::DeleteEntityAck", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(success ? 1 : 0));
        w.write_string(error);
    }

    static auto deserialize(BinaryReader& r) -> Result<DeleteEntityAck>
    {
        auto rid = r.read<uint32_t>();
        auto ok = r.read<uint8_t>();
        auto err = r.read_string();
        if (!rid || !ok || !err)
            return Error{ErrorCode::InvalidArgument, "DeleteEntityAck: truncated"};
        DeleteEntityAck msg;
        msg.request_id = *rid;
        msg.success = *ok != 0;
        msg.error = std::move(*err);
        return msg;
    }
};
static_assert(NetworkMessage<DeleteEntityAck>);

// ============================================================================
// LookupEntity  (BaseApp → DBApp, ID 4007)
// ============================================================================

struct LookupEntity
{
    uint16_t type_id{0};
    std::string identifier;
    uint32_t request_id{0};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::LookupEntity),
                                      "dbapp::LookupEntity", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(type_id);
        w.write_string(identifier);
        w.write(request_id);
    }

    static auto deserialize(BinaryReader& r) -> Result<LookupEntity>
    {
        auto ti = r.read<uint16_t>();
        auto id_str = r.read_string();
        auto rid = r.read<uint32_t>();
        if (!ti || !id_str || !rid)
            return Error{ErrorCode::InvalidArgument, "LookupEntity: truncated"};
        LookupEntity msg;
        msg.type_id = *ti;
        msg.identifier = std::move(*id_str);
        msg.request_id = *rid;
        return msg;
    }
};
static_assert(NetworkMessage<LookupEntity>);

// ============================================================================
// LookupEntityAck  (DBApp → BaseApp, ID 4008)
// ============================================================================

struct LookupEntityAck
{
    uint32_t request_id{0};
    bool found{false};
    DatabaseID dbid{kInvalidDBID};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::LookupEntityAck),
                                      "dbapp::LookupEntityAck", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint32_t) + 1 + sizeof(int64_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(found ? 1 : 0));
        w.write(dbid);
    }

    static auto deserialize(BinaryReader& r) -> Result<LookupEntityAck>
    {
        auto rid = r.read<uint32_t>();
        auto f = r.read<uint8_t>();
        auto db = r.read<int64_t>();
        if (!rid || !f || !db)
            return Error{ErrorCode::InvalidArgument, "LookupEntityAck: truncated"};
        LookupEntityAck msg;
        msg.request_id = *rid;
        msg.found = *f != 0;
        msg.dbid = *db;
        return msg;
    }
};
static_assert(NetworkMessage<LookupEntityAck>);

// ============================================================================
// AbortCheckout  (BaseApp → DBApp, ID 4009)
// Cancel a previously issued CheckoutEntity request that is no longer needed.
// ============================================================================

struct AbortCheckout
{
    uint32_t request_id{0};
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{
            msg_id::id(msg_id::DBApp::AbortCheckout), "dbapp::AbortCheckout",
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

    static auto deserialize(BinaryReader& r) -> Result<AbortCheckout>
    {
        auto rid = r.read<uint32_t>();
        auto ti = r.read<uint16_t>();
        auto db = r.read<int64_t>();
        if (!rid || !ti || !db)
            return Error{ErrorCode::InvalidArgument, "AbortCheckout: truncated"};
        AbortCheckout msg;
        msg.request_id = *rid;
        msg.type_id = *ti;
        msg.dbid = *db;
        return msg;
    }
};
static_assert(NetworkMessage<AbortCheckout>);

// ============================================================================
// AbortCheckoutAck  (DBApp → BaseApp, ID 4010)
// ============================================================================

struct AbortCheckoutAck
{
    uint32_t request_id{0};
    bool success{false};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::DBApp::AbortCheckoutAck),
                                      "dbapp::AbortCheckoutAck", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(success ? 1 : 0));
    }

    static auto deserialize(BinaryReader& r) -> Result<AbortCheckoutAck>
    {
        auto rid = r.read<uint32_t>();
        auto ok = r.read<uint8_t>();
        if (!rid || !ok)
            return Error{ErrorCode::InvalidArgument, "AbortCheckoutAck: truncated"};
        AbortCheckoutAck msg;
        msg.request_id = *rid;
        msg.success = (*ok != 0);
        return msg;
    }
};
static_assert(NetworkMessage<AbortCheckoutAck>);

}  // namespace atlas::dbapp

// ============================================================================
// Authentication messages delegated to DBApp (IDs 5002–5003 reused here)
// These are the same message types defined in login_messages.hpp, but
// re-exported in this header so DBApp only needs to include one file.
// DBApp registers handlers using the same atlas::login:: types.
//
// To avoid ODR issues, do NOT include both headers in the same translation
// unit.  In practice, DBApp includes only this header for auth messages.
// ============================================================================

// AuthLogin / AuthLoginResult are defined in loginapp/login_messages.hpp.
// DBApp includes that header directly when handling auth messages.
