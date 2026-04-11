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
// BaseApp messages (IDs 2000–2031)
//
// External interface (client ↔ BaseApp): 2020–2029
// Internal interface (peer servers ↔ BaseApp): 2000–2019, 2030–2031
//
// Sources:
//   BaseAppMgr → BaseApp : CreateBase, CreateBaseFromDB
//   BaseApp    → BaseApp : AcceptClient
//   CellApp    → BaseApp : CellEntityCreated, CellEntityDestroyed, CurrentCell,
//                           CellRpcForward, SelfRpcFromCell, BroadcastRpcFromCell,
//                           ReplicatedDeltaFromCell
//   DBApp      → BaseApp : WriteEntityAck (re-used from dbapp_messages.hpp)
// ============================================================================

namespace atlas::baseapp
{

// ============================================================================
// CreateBase  (BaseAppMgr → BaseApp, ID 2000)
// ============================================================================

struct CreateBase
{
    uint16_t type_id{0};
    EntityID entity_id{kInvalidEntityID};  // 0 = BaseApp allocates locally

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::CreateBase),
                                      "baseapp::CreateBase", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint16_t) + sizeof(uint32_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(type_id);
        w.write(entity_id);
    }

    static auto deserialize(BinaryReader& r) -> Result<CreateBase>
    {
        auto ti = r.read<uint16_t>();
        auto eid = r.read<uint32_t>();
        if (!ti || !eid)
            return Error{ErrorCode::InvalidArgument, "CreateBase: truncated"};
        CreateBase msg;
        msg.type_id = *ti;
        msg.entity_id = *eid;
        return msg;
    }
};
static_assert(NetworkMessage<CreateBase>);

// ============================================================================
// CreateBaseFromDB  (BaseAppMgr → BaseApp, ID 2001)
// ============================================================================

struct CreateBaseFromDB
{
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};
    std::string identifier;  // non-empty → load by name

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::CreateBaseFromDB),
                                      "baseapp::CreateBaseFromDB", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(type_id);
        w.write(dbid);
        w.write_string(identifier);
    }

    static auto deserialize(BinaryReader& r) -> Result<CreateBaseFromDB>
    {
        auto ti = r.read<uint16_t>();
        auto db = r.read<int64_t>();
        auto id = r.read_string();
        if (!ti || !db || !id)
            return Error{ErrorCode::InvalidArgument, "CreateBaseFromDB: truncated"};
        CreateBaseFromDB msg;
        msg.type_id = *ti;
        msg.dbid = *db;
        msg.identifier = std::move(*id);
        return msg;
    }
};
static_assert(NetworkMessage<CreateBaseFromDB>);

// ============================================================================
// AcceptClient  (BaseApp → BaseApp, remote give_client_to, ID 2002)
// ============================================================================

struct AcceptClient
{
    EntityID dest_entity_id{kInvalidEntityID};
    SessionKey session_key;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::AcceptClient),
                                      "baseapp::AcceptClient", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint32_t) + sizeof(SessionKey))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(dest_entity_id);
        w.write_bytes(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(session_key.bytes), sizeof(session_key.bytes)));
    }

    static auto deserialize(BinaryReader& r) -> Result<AcceptClient>
    {
        auto eid = r.read<uint32_t>();
        if (!eid)
            return Error{ErrorCode::InvalidArgument, "AcceptClient: truncated"};
        auto key_span = r.read_bytes(sizeof(SessionKey));
        if (!key_span)
            return Error{ErrorCode::InvalidArgument, "AcceptClient: key truncated"};
        AcceptClient msg;
        msg.dest_entity_id = *eid;
        std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
        return msg;
    }
};
static_assert(NetworkMessage<AcceptClient>);

// ============================================================================
// CellEntityCreated  (CellApp → BaseApp, ID 2010)
// ============================================================================

struct CellEntityCreated
{
    EntityID base_entity_id{kInvalidEntityID};
    EntityID cell_entity_id{kInvalidEntityID};
    Address cell_addr;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{
            msg_id::id(msg_id::BaseApp::CellEntityCreated), "baseapp::CellEntityCreated",
            MessageLengthStyle::Fixed,
            static_cast<int>(sizeof(uint32_t) * 2 + sizeof(uint32_t) + sizeof(uint16_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(base_entity_id);
        w.write(cell_entity_id);
        w.write(cell_addr.ip());
        w.write(cell_addr.port());
    }

    static auto deserialize(BinaryReader& r) -> Result<CellEntityCreated>
    {
        auto beid = r.read<uint32_t>();
        auto ceid = r.read<uint32_t>();
        auto ip = r.read<uint32_t>();
        auto port = r.read<uint16_t>();
        if (!beid || !ceid || !ip || !port)
            return Error{ErrorCode::InvalidArgument, "CellEntityCreated: truncated"};
        CellEntityCreated msg;
        msg.base_entity_id = *beid;
        msg.cell_entity_id = *ceid;
        msg.cell_addr = Address(*ip, *port);
        return msg;
    }
};
static_assert(NetworkMessage<CellEntityCreated>);

// ============================================================================
// CellEntityDestroyed  (CellApp → BaseApp, ID 2011)
// ============================================================================

struct CellEntityDestroyed
{
    EntityID base_entity_id{kInvalidEntityID};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::CellEntityDestroyed),
                                      "baseapp::CellEntityDestroyed", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint32_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write(base_entity_id); }

    static auto deserialize(BinaryReader& r) -> Result<CellEntityDestroyed>
    {
        auto eid = r.read<uint32_t>();
        if (!eid)
            return Error{ErrorCode::InvalidArgument, "CellEntityDestroyed: truncated"};
        CellEntityDestroyed msg;
        msg.base_entity_id = *eid;
        return msg;
    }
};
static_assert(NetworkMessage<CellEntityDestroyed>);

// ============================================================================
// CurrentCell  (CellApp → BaseApp, cell address updated after offload, ID 2012)
// ============================================================================

struct CurrentCell
{
    EntityID base_entity_id{kInvalidEntityID};
    EntityID cell_entity_id{kInvalidEntityID};
    Address cell_addr;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{
            msg_id::id(msg_id::BaseApp::CurrentCell), "baseapp::CurrentCell",
            MessageLengthStyle::Fixed,
            static_cast<int>(sizeof(uint32_t) * 2 + sizeof(uint32_t) + sizeof(uint16_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(base_entity_id);
        w.write(cell_entity_id);
        w.write(cell_addr.ip());
        w.write(cell_addr.port());
    }

    static auto deserialize(BinaryReader& r) -> Result<CurrentCell>
    {
        auto beid = r.read<uint32_t>();
        auto ceid = r.read<uint32_t>();
        auto ip = r.read<uint32_t>();
        auto port = r.read<uint16_t>();
        if (!beid || !ceid || !ip || !port)
            return Error{ErrorCode::InvalidArgument, "CurrentCell: truncated"};
        CurrentCell msg;
        msg.base_entity_id = *beid;
        msg.cell_entity_id = *ceid;
        msg.cell_addr = Address(*ip, *port);
        return msg;
    }
};
static_assert(NetworkMessage<CurrentCell>);

// ============================================================================
// CellRpcForward  (CellApp → BaseApp, Cell→Base RPC, ID 2013)
// ============================================================================

struct CellRpcForward
{
    EntityID base_entity_id{kInvalidEntityID};
    uint32_t rpc_id{0};
    std::vector<std::byte> payload;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::CellRpcForward),
                                      "baseapp::CellRpcForward", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_packed_int(base_entity_id);
        w.write_packed_int(rpc_id);
        w.write_packed_int(static_cast<uint32_t>(payload.size()));
        w.write_bytes(payload);
    }

    static auto deserialize(BinaryReader& r) -> Result<CellRpcForward>
    {
        auto eid = r.read_packed_int();
        auto rid = r.read_packed_int();
        auto sz = r.read_packed_int();
        if (!eid || !rid || !sz)
            return Error{ErrorCode::InvalidArgument, "CellRpcForward: truncated"};
        auto span = r.read_bytes(*sz);
        if (!span)
            return Error{ErrorCode::InvalidArgument, "CellRpcForward: payload truncated"};
        CellRpcForward msg;
        msg.base_entity_id = *eid;
        msg.rpc_id = *rid;
        msg.payload.assign(span->begin(), span->end());
        return msg;
    }
};
static_assert(NetworkMessage<CellRpcForward>);

// ============================================================================
// SelfRpcFromCell  (CellApp → BaseApp → owning client, ID 2014)
//
// RPC directed at the entity's own client (target == self).
// Must be delivered reliably — loss causes permanent client state desync
// (e.g. skill hit confirmation, damage numbers, state transitions).
// ============================================================================

struct SelfRpcFromCell
{
    EntityID base_entity_id{kInvalidEntityID};
    uint32_t rpc_id{0};
    std::vector<std::byte> payload;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::SelfRpcFromCell),
                                      "baseapp::SelfRpcFromCell", MessageLengthStyle::Variable, -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_packed_int(base_entity_id);
        w.write_packed_int(rpc_id);
        w.write_packed_int(static_cast<uint32_t>(payload.size()));
        w.write_bytes(payload);
    }

    static auto deserialize(BinaryReader& r) -> Result<SelfRpcFromCell>
    {
        auto eid = r.read_packed_int();
        auto rid = r.read_packed_int();
        auto sz = r.read_packed_int();
        if (!eid || !rid || !sz)
            return Error{ErrorCode::InvalidArgument, "SelfRpcFromCell: truncated"};
        auto span = r.read_bytes(*sz);
        if (!span)
            return Error{ErrorCode::InvalidArgument, "SelfRpcFromCell: payload truncated"};
        SelfRpcFromCell msg;
        msg.base_entity_id = *eid;
        msg.rpc_id = *rid;
        msg.payload.assign(span->begin(), span->end());
        return msg;
    }
};
static_assert(NetworkMessage<SelfRpcFromCell>);

// ============================================================================
// BroadcastRpcFromCell  (CellApp → BaseApp → nearby clients, ID 2016)
//
// RPC broadcast to otherClients or allClients.
// High-frequency (animations, movement events); a newer frame supersedes the
// old one, so best-effort delivery is preferred over retransmit latency.
// ============================================================================

struct BroadcastRpcFromCell
{
    EntityID base_entity_id{kInvalidEntityID};
    uint32_t rpc_id{0};
    uint8_t target{1};  // 1=otherClients, 2=allClients
    std::vector<std::byte> payload;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::BroadcastRpcFromCell),
                                      "baseapp::BroadcastRpcFromCell", MessageLengthStyle::Variable,
                                      -1, MessageReliability::Unreliable};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_packed_int(base_entity_id);
        w.write_packed_int(rpc_id);
        w.write(target);
        w.write_packed_int(static_cast<uint32_t>(payload.size()));
        w.write_bytes(payload);
    }

    static auto deserialize(BinaryReader& r) -> Result<BroadcastRpcFromCell>
    {
        auto eid = r.read_packed_int();
        auto rid = r.read_packed_int();
        auto tgt = r.read<uint8_t>();
        auto sz = r.read_packed_int();
        if (!eid || !rid || !tgt || !sz)
            return Error{ErrorCode::InvalidArgument, "BroadcastRpcFromCell: truncated"};
        auto span = r.read_bytes(*sz);
        if (!span)
            return Error{ErrorCode::InvalidArgument, "BroadcastRpcFromCell: payload truncated"};
        BroadcastRpcFromCell msg;
        msg.base_entity_id = *eid;
        msg.rpc_id = *rid;
        msg.target = *tgt;
        msg.payload.assign(span->begin(), span->end());
        return msg;
    }
};
static_assert(NetworkMessage<BroadcastRpcFromCell>);

// ============================================================================
// ReplicatedDeltaFromCell  (CellApp → BaseApp → Client, ID 2015)
// ============================================================================

struct ReplicatedDeltaFromCell
{
    EntityID base_entity_id{kInvalidEntityID};
    std::vector<std::byte> delta;

    // AoI / property replication deltas are superseded by the next tick —
    // best-effort delivery is preferred over head-of-line blocking.
    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::ReplicatedDeltaFromCell),
                                      "baseapp::ReplicatedDeltaFromCell",
                                      MessageLengthStyle::Variable, -1,
                                      MessageReliability::Unreliable};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_packed_int(base_entity_id);
        w.write_packed_int(static_cast<uint32_t>(delta.size()));
        w.write_bytes(delta);
    }

    static auto deserialize(BinaryReader& r) -> Result<ReplicatedDeltaFromCell>
    {
        auto eid = r.read_packed_int();
        auto sz = r.read_packed_int();
        if (!eid || !sz)
            return Error{ErrorCode::InvalidArgument, "ReplicatedDeltaFromCell: truncated"};
        auto span = r.read_bytes(*sz);
        if (!span)
            return Error{ErrorCode::InvalidArgument, "ReplicatedDeltaFromCell: delta truncated"};
        ReplicatedDeltaFromCell msg;
        msg.base_entity_id = *eid;
        msg.delta.assign(span->begin(), span->end());
        return msg;
    }
};
static_assert(NetworkMessage<ReplicatedDeltaFromCell>);

// ============================================================================
// Authenticate  (Client → BaseApp external interface, ID 2020)
// First message sent by client after connecting to BaseApp.
// ============================================================================

struct Authenticate
{
    SessionKey session_key;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::Authenticate),
                                      "baseapp::Authenticate", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(SessionKey))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write_bytes(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(session_key.bytes), sizeof(session_key.bytes)));
    }

    static auto deserialize(BinaryReader& r) -> Result<Authenticate>
    {
        auto key_span = r.read_bytes(sizeof(SessionKey));
        if (!key_span)
            return Error{ErrorCode::InvalidArgument, "Authenticate: key truncated"};
        Authenticate msg;
        std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
        return msg;
    }
};
static_assert(NetworkMessage<Authenticate>);

// ============================================================================
// AuthenticateResult  (BaseApp → Client, ID 2021)
// ============================================================================

struct AuthenticateResult
{
    bool success{false};
    EntityID entity_id{kInvalidEntityID};
    uint16_t type_id{0};
    std::string error;

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::AuthenticateResult),
                                      "baseapp::AuthenticateResult", MessageLengthStyle::Variable,
                                      -1};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(static_cast<uint8_t>(success ? 1 : 0));
        w.write(entity_id);
        w.write(type_id);
        w.write_string(error);
    }

    static auto deserialize(BinaryReader& r) -> Result<AuthenticateResult>
    {
        auto ok = r.read<uint8_t>();
        auto eid = r.read<uint32_t>();
        auto ti = r.read<uint16_t>();
        auto err = r.read_string();
        if (!ok || !eid || !ti || !err)
            return Error{ErrorCode::InvalidArgument, "AuthenticateResult: truncated"};
        AuthenticateResult msg;
        msg.success = (*ok != 0);
        msg.entity_id = *eid;
        msg.type_id = *ti;
        msg.error = std::move(*err);
        return msg;
    }
};
static_assert(NetworkMessage<AuthenticateResult>);

// ============================================================================
// ForceLogoff  (BaseApp → BaseApp, ID 2030)
// Sent to evict an existing Proxy so the new login can checkout the entity.
// ============================================================================

struct ForceLogoff
{
    DatabaseID dbid{kInvalidDBID};
    uint32_t request_id{0};  // echoed back in ForceLogoffAck

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::ForceLogoff),
                                      "baseapp::ForceLogoff", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(int64_t) + sizeof(uint32_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(dbid);
        w.write(request_id);
    }

    static auto deserialize(BinaryReader& r) -> Result<ForceLogoff>
    {
        auto db = r.read<int64_t>();
        auto rid = r.read<uint32_t>();
        if (!db || !rid)
            return Error{ErrorCode::InvalidArgument, "ForceLogoff: truncated"};
        ForceLogoff msg;
        msg.dbid = *db;
        msg.request_id = *rid;
        return msg;
    }
};
static_assert(NetworkMessage<ForceLogoff>);

// ============================================================================
// ForceLogoffAck  (BaseApp → BaseApp, ID 2031)
// ============================================================================

struct ForceLogoffAck
{
    uint32_t request_id{0};
    bool success{false};

    static auto descriptor() -> const MessageDesc&
    {
        static const MessageDesc desc{msg_id::id(msg_id::BaseApp::ForceLogoffAck),
                                      "baseapp::ForceLogoffAck", MessageLengthStyle::Fixed,
                                      static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t))};
        return desc;
    }

    void serialize(BinaryWriter& w) const
    {
        w.write(request_id);
        w.write(static_cast<uint8_t>(success ? 1 : 0));
    }

    static auto deserialize(BinaryReader& r) -> Result<ForceLogoffAck>
    {
        auto rid = r.read<uint32_t>();
        auto ok = r.read<uint8_t>();
        if (!rid || !ok)
            return Error{ErrorCode::InvalidArgument, "ForceLogoffAck: truncated"};
        ForceLogoffAck msg;
        msg.request_id = *rid;
        msg.success = (*ok != 0);
        return msg;
    }
};
static_assert(NetworkMessage<ForceLogoffAck>);

}  // namespace atlas::baseapp
