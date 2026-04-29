#ifndef ATLAS_SERVER_BASEAPP_BASEAPP_MESSAGES_H_
#define ATLAS_SERVER_BASEAPP_BASEAPP_MESSAGES_H_

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "db/idatabase.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "server/entity_types.h"

// BaseApp messages (IDs 2000-2031). External 2020-2029, internal otherwise.

namespace atlas::baseapp {

// CreateBase (BaseAppMgr → BaseApp, ID 2000).
struct CreateBase {
  uint16_t type_id{0};
  EntityID entity_id{kInvalidEntityID};  // 0 = BaseApp allocates locally

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCreateBase),
                                   "baseapp::CreateBase",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint16_t) + sizeof(uint32_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(type_id);
    w.Write(entity_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CreateBase> {
    auto ti = r.Read<uint16_t>();
    auto eid = r.Read<uint32_t>();
    if (!ti || !eid) return Error{ErrorCode::kInvalidArgument, "CreateBase: truncated"};
    CreateBase msg;
    msg.type_id = *ti;
    msg.entity_id = *eid;
    return msg;
  }
};
static_assert(NetworkMessage<CreateBase>);

// CreateBaseFromDB (BaseAppMgr → BaseApp, ID 2001).
struct CreateBaseFromDB {
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};
  std::string identifier;  // non-empty → load by name

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCreateBaseFromDb),
                                   "baseapp::CreateBaseFromDB",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(type_id);
    w.Write(dbid);
    w.WriteString(identifier);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CreateBaseFromDB> {
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    auto id = r.ReadString();
    if (!ti || !db || !id) return Error{ErrorCode::kInvalidArgument, "CreateBaseFromDB: truncated"};
    CreateBaseFromDB msg;
    msg.type_id = *ti;
    msg.dbid = *db;
    msg.identifier = std::move(*id);
    return msg;
  }
};
static_assert(NetworkMessage<CreateBaseFromDB>);

// AcceptClient (BaseApp → BaseApp, remote give_client_to, ID 2002).
struct AcceptClient {
  EntityID dest_entity_id{kInvalidEntityID};
  SessionKey session_key;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kAcceptClient),
                                   "baseapp::AcceptClient",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(SessionKey)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(dest_entity_id);
    w.WriteBytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(session_key.bytes),
                                            sizeof(session_key.bytes)));
  }

  static auto Deserialize(BinaryReader& r) -> Result<AcceptClient> {
    auto eid = r.Read<uint32_t>();
    if (!eid) return Error{ErrorCode::kInvalidArgument, "AcceptClient: truncated"};
    auto key_span = r.ReadBytes(sizeof(SessionKey));
    if (!key_span) return Error{ErrorCode::kInvalidArgument, "AcceptClient: key truncated"};
    AcceptClient msg;
    msg.dest_entity_id = *eid;
    std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
    return msg;
  }
};
static_assert(NetworkMessage<AcceptClient>);

// CellEntityCreated (CellApp → BaseApp, ID 2010).
struct CellEntityCreated {
  EntityID base_entity_id{kInvalidEntityID};
  EntityID cell_entity_id{kInvalidEntityID};
  Address cell_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseApp::kCellEntityCreated),
        "baseapp::CellEntityCreated",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) * 2 + sizeof(uint32_t) + sizeof(uint16_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(base_entity_id);
    w.Write(cell_entity_id);
    w.Write(cell_addr.Ip());
    w.Write(cell_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<CellEntityCreated> {
    auto beid = r.Read<uint32_t>();
    auto ceid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    if (!beid || !ceid || !ip || !port)
      return Error{ErrorCode::kInvalidArgument, "CellEntityCreated: truncated"};
    CellEntityCreated msg;
    msg.base_entity_id = *beid;
    msg.cell_entity_id = *ceid;
    msg.cell_addr = Address(*ip, *port);
    return msg;
  }
};
static_assert(NetworkMessage<CellEntityCreated>);

// CellEntityDestroyed (CellApp → BaseApp, ID 2011).
struct CellEntityDestroyed {
  EntityID base_entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellEntityDestroyed),
                                   "baseapp::CellEntityDestroyed",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(base_entity_id); }

  static auto Deserialize(BinaryReader& r) -> Result<CellEntityDestroyed> {
    auto eid = r.Read<uint32_t>();
    if (!eid) return Error{ErrorCode::kInvalidArgument, "CellEntityDestroyed: truncated"};
    CellEntityDestroyed msg;
    msg.base_entity_id = *eid;
    return msg;
  }
};
static_assert(NetworkMessage<CellEntityDestroyed>);

// CurrentCell (CellApp → BaseApp, post-offload cell address, ID 2012).
struct CurrentCell {
  EntityID base_entity_id{kInvalidEntityID};
  EntityID cell_entity_id{kInvalidEntityID};
  Address cell_addr;
  uint32_t epoch{0};  // Monotonic; BaseApp rejects stale updates (epoch < stored).

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseApp::kCurrentCell),
        "baseapp::CurrentCell",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) * 3 + sizeof(uint32_t) + sizeof(uint16_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(base_entity_id);
    w.Write(cell_entity_id);
    w.Write(cell_addr.Ip());
    w.Write(cell_addr.Port());
    w.Write(epoch);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CurrentCell> {
    auto beid = r.Read<uint32_t>();
    auto ceid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto ep = r.Read<uint32_t>();
    if (!beid || !ceid || !ip || !port || !ep)
      return Error{ErrorCode::kInvalidArgument, "CurrentCell: truncated"};
    CurrentCell msg;
    msg.base_entity_id = *beid;
    msg.cell_entity_id = *ceid;
    msg.cell_addr = Address(*ip, *port);
    msg.epoch = *ep;
    return msg;
  }
};
static_assert(NetworkMessage<CurrentCell>);

// CellRpcForward (CellApp → BaseApp, Cell→Base RPC, ID 2013).
struct CellRpcForward {
  EntityID base_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellRpcForward),
                                   "baseapp::CellRpcForward",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(rpc_id);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    w.WriteBytes(payload);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CellRpcForward> {
    auto eid = r.ReadPackedInt();
    auto rid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !rid || !sz) return Error{ErrorCode::kInvalidArgument, "CellRpcForward: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span) return Error{ErrorCode::kInvalidArgument, "CellRpcForward: payload truncated"};
    CellRpcForward msg;
    msg.base_entity_id = *eid;
    msg.rpc_id = *rid;
    msg.payload.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<CellRpcForward>);

// Cell-scope-resolved RPC fan-out (ID 2016); BaseApp dispatches to each
// destination's bound client via the unified RPC envelope.
struct BroadcastRpcFromCell {
  uint32_t rpc_id{0};
  std::vector<EntityID> dest_entity_ids;
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kBroadcastRpcFromCell),
                                   "baseapp::BroadcastRpcFromCell",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(rpc_id);
    w.WritePackedInt(static_cast<uint32_t>(dest_entity_ids.size()));
    for (auto id : dest_entity_ids) w.WritePackedInt(id);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    w.WriteBytes(payload);
  }

  static auto Deserialize(BinaryReader& r) -> Result<BroadcastRpcFromCell> {
    auto rid = r.ReadPackedInt();
    auto count = r.ReadPackedInt();
    if (!rid || !count)
      return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: truncated header"};
    BroadcastRpcFromCell msg;
    msg.rpc_id = *rid;
    msg.dest_entity_ids.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto id = r.ReadPackedInt();
      if (!id) return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: dest list truncated"};
      msg.dest_entity_ids.push_back(*id);
    }
    auto sz = r.ReadPackedInt();
    if (!sz) return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: missing payload size"};
    auto span = r.ReadBytes(*sz);
    if (!span) return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: payload truncated"};
    msg.payload.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<BroadcastRpcFromCell>);

// ReplicatedDeltaFromCell (CellApp → BaseApp → Client, ID 2015).
// Unreliable: next tick supersedes; HoL blocking is worse than loss.
struct ReplicatedDeltaFromCell {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> delta;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kReplicatedDeltaFromCell),
                                   "baseapp::ReplicatedDeltaFromCell",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kUnreliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(static_cast<uint32_t>(delta.size()));
    w.WriteBytes(delta);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ReplicatedDeltaFromCell> {
    auto eid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !sz)
      return Error{ErrorCode::kInvalidArgument, "ReplicatedDeltaFromCell: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span)
      return Error{ErrorCode::kInvalidArgument, "ReplicatedDeltaFromCell: delta truncated"};
    ReplicatedDeltaFromCell msg;
    msg.base_entity_id = *eid;
    msg.delta.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<ReplicatedDeltaFromCell>);

// Send-only zero-copy view of ReplicatedDeltaFromCell. Saves ~3 µs/observer
// at 200 obs/tick by skipping per-observer std::vector::assign.
struct ReplicatedDeltaFromCellSpan {
  EntityID base_entity_id{kInvalidEntityID};
  std::span<const std::byte> delta;

  static auto Descriptor() -> const MessageDesc& { return ReplicatedDeltaFromCell::Descriptor(); }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(static_cast<uint32_t>(delta.size()));
    w.WriteBytes(delta);
  }

  // Send-only stub to satisfy NetworkMessage concept.
  static auto Deserialize(BinaryReader&) -> Result<ReplicatedDeltaFromCellSpan> {
    return Error{ErrorCode::kInvalidArgument, "ReplicatedDeltaFromCellSpan is send-only"};
  }
};
static_assert(NetworkMessage<ReplicatedDeltaFromCellSpan>);

// Reliable twin of ReplicatedDeltaFromCell (ID 2017). For reliable="true"
// fields (HP, state, inventory). Bypasses DeltaForwarder budget; same wire
// format so the client reuses ApplyReplicatedDelta.
struct ReplicatedReliableDeltaFromCell {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> delta;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kReplicatedReliableDeltaFromCell),
                                   "baseapp::ReplicatedReliableDeltaFromCell",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(static_cast<uint32_t>(delta.size()));
    w.WriteBytes(delta);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ReplicatedReliableDeltaFromCell> {
    auto eid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !sz)
      return Error{ErrorCode::kInvalidArgument, "ReplicatedReliableDeltaFromCell: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span)
      return Error{ErrorCode::kInvalidArgument, "ReplicatedReliableDeltaFromCell: delta truncated"};
    ReplicatedReliableDeltaFromCell msg;
    msg.base_entity_id = *eid;
    msg.delta.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<ReplicatedReliableDeltaFromCell>);

// Send-only zero-copy view; see ReplicatedDeltaFromCellSpan.
struct ReplicatedReliableDeltaFromCellSpan {
  EntityID base_entity_id{kInvalidEntityID};
  std::span<const std::byte> delta;

  static auto Descriptor() -> const MessageDesc& {
    return ReplicatedReliableDeltaFromCell::Descriptor();
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(static_cast<uint32_t>(delta.size()));
    w.WriteBytes(delta);
  }

  static auto Deserialize(BinaryReader&) -> Result<ReplicatedReliableDeltaFromCellSpan> {
    return Error{ErrorCode::kInvalidArgument, "ReplicatedReliableDeltaFromCellSpan is send-only"};
  }
};
static_assert(NetworkMessage<ReplicatedReliableDeltaFromCellSpan>);

// Periodic cell-to-base state backup (ID 2018); opaque CELL_DATA bytes.
// BaseApp stores verbatim for DB writes / Reviver / Offload bootstrap.
struct BackupCellEntity {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> cell_backup_data;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kBackupCellEntity),
                                   "baseapp::BackupCellEntity",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(static_cast<uint32_t>(cell_backup_data.size()));
    w.WriteBytes(cell_backup_data);
  }

  static auto Deserialize(BinaryReader& r) -> Result<BackupCellEntity> {
    auto eid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !sz) return Error{ErrorCode::kInvalidArgument, "BackupCellEntity: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span) return Error{ErrorCode::kInvalidArgument, "BackupCellEntity: blob truncated"};
    BackupCellEntity msg;
    msg.base_entity_id = *eid;
    msg.cell_backup_data.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<BackupCellEntity>);

// Periodic cell-authoritative owner snapshot (ID 2019). BaseApp relays as
// ReplicatedBaselineToClient (0xF002).
struct ReplicatedBaselineFromCell {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> snapshot;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kReplicatedBaselineFromCell),
                                   "baseapp::ReplicatedBaselineFromCell",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(static_cast<uint32_t>(snapshot.size()));
    w.WriteBytes(snapshot);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ReplicatedBaselineFromCell> {
    auto eid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !sz)
      return Error{ErrorCode::kInvalidArgument, "ReplicatedBaselineFromCell: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span)
      return Error{ErrorCode::kInvalidArgument, "ReplicatedBaselineFromCell: snapshot truncated"};
    ReplicatedBaselineFromCell msg;
    msg.base_entity_id = *eid;
    msg.snapshot.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<ReplicatedBaselineFromCell>);

// Periodic owner-snapshot to client (client-facing ID 0xF002). Reliable;
// recovers any property lost on the unreliable delta path.
struct ReplicatedBaselineToClient {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> snapshot;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        static_cast<MessageID>(0xF002), "baseapp::ReplicatedBaselineToClient",
        MessageLengthStyle::kVariable, -1, MessageReliability::kReliable};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(static_cast<uint32_t>(snapshot.size()));
    w.WriteBytes(snapshot);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ReplicatedBaselineToClient> {
    auto eid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !sz)
      return Error{ErrorCode::kInvalidArgument, "ReplicatedBaselineToClient: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span)
      return Error{ErrorCode::kInvalidArgument, "ReplicatedBaselineToClient: snapshot truncated"};
    ReplicatedBaselineToClient msg;
    msg.base_entity_id = *eid;
    msg.snapshot.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<ReplicatedBaselineToClient>);

// Authenticate (Client → BaseApp external, ID 2020); first client message.
struct Authenticate {
  SessionKey session_key;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kAuthenticate),
                                   "baseapp::Authenticate",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(SessionKey)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WriteBytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(session_key.bytes),
                                            sizeof(session_key.bytes)));
  }

  static auto Deserialize(BinaryReader& r) -> Result<Authenticate> {
    auto key_span = r.ReadBytes(sizeof(SessionKey));
    if (!key_span) return Error{ErrorCode::kInvalidArgument, "Authenticate: key truncated"};
    Authenticate msg;
    std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
    return msg;
  }
};
static_assert(NetworkMessage<Authenticate>);

// AuthenticateResult (BaseApp → Client, ID 2021).
struct AuthenticateResult {
  bool success{false};
  EntityID entity_id{kInvalidEntityID};
  uint16_t type_id{0};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kAuthenticateResult),
                                   "baseapp::AuthenticateResult",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(entity_id);
    w.Write(type_id);
    w.WriteString(error);
  }

  static auto Deserialize(BinaryReader& r) -> Result<AuthenticateResult> {
    auto ok = r.Read<uint8_t>();
    auto eid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    auto err = r.ReadString();
    if (!ok || !eid || !ti || !err)
      return Error{ErrorCode::kInvalidArgument, "AuthenticateResult: truncated"};
    AuthenticateResult msg;
    msg.success = (*ok != 0);
    msg.entity_id = *eid;
    msg.type_id = *ti;
    msg.error = std::move(*err);
    return msg;
  }
};
static_assert(NetworkMessage<AuthenticateResult>);

// Owning-entity change after server-side GiveClientTo (ID 2024). Client
// must use new_entity_id for subsequent ClientCellRpc; previous id is
// unbound and will fail validation.
struct EntityTransferred {
  EntityID new_entity_id{kInvalidEntityID};
  uint16_t new_type_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kEntityTransferred),
                                   "baseapp::EntityTransferred",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(new_entity_id);
    w.Write(new_type_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<EntityTransferred> {
    auto eid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    if (!eid || !ti) return Error{ErrorCode::kInvalidArgument, "EntityTransferred: truncated"};
    EntityTransferred msg;
    msg.new_entity_id = *eid;
    msg.new_type_id = *ti;
    return msg;
  }
};
static_assert(NetworkMessage<EntityTransferred>);

// Tells client its cell is ready (ID 2025); fires once both BindClient
// and SetCell have happened. Avoids race where ClientCellRpc would be
// dropped for missing cell_addr.
struct CellReady {
  EntityID entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellReady),
                                   "baseapp::CellReady",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(entity_id); }

  static auto Deserialize(BinaryReader& r) -> Result<CellReady> {
    auto eid = r.Read<uint32_t>();
    if (!eid) return Error{ErrorCode::kInvalidArgument, "CellReady: truncated"};
    CellReady msg;
    msg.entity_id = *eid;
    return msg;
  }
};
static_assert(NetworkMessage<CellReady>);

// ClientBaseRpc (Client → BaseApp external, ID 2022); base method call.
struct ClientBaseRpc {
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kClientBaseRpc),
                                   "baseapp::ClientBaseRpc",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(rpc_id);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) w.WriteBytes(std::span<const std::byte>(payload));
  }

  static auto Deserialize(BinaryReader& r) -> Result<ClientBaseRpc> {
    auto rid = r.Read<uint32_t>();
    auto plen = r.ReadPackedInt();
    if (!rid || !plen) return Error{ErrorCode::kInvalidArgument, "ClientBaseRpc: truncated"};
    ClientBaseRpc msg;
    msg.rpc_id = *rid;
    if (*plen > 0) {
      auto pdata = r.ReadBytes(*plen);
      if (!pdata) return Error{ErrorCode::kInvalidArgument, "ClientBaseRpc: payload truncated"};
      msg.payload.assign(pdata->begin(), pdata->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<ClientBaseRpc>);

// ClientCellRpc (Client → BaseApp external, ID 2023). BaseApp validates
// then forwards to CellApp with un-spoofable source_entity_id stamped.
// target_entity_id is in base_entity_id space.
struct ClientCellRpc {
  EntityID target_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kClientCellRpc),
                                   "baseapp::ClientCellRpc",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(target_entity_id);
    w.Write(rpc_id);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) w.WriteBytes(std::span<const std::byte>(payload));
  }

  static auto Deserialize(BinaryReader& r) -> Result<ClientCellRpc> {
    auto tid = r.ReadPackedInt();
    auto rid = r.Read<uint32_t>();
    auto plen = r.ReadPackedInt();
    if (!tid || !rid || !plen)
      return Error{ErrorCode::kInvalidArgument, "ClientCellRpc: truncated"};
    ClientCellRpc msg;
    msg.target_entity_id = *tid;
    msg.rpc_id = *rid;
    if (*plen > 0) {
      auto pdata = r.ReadBytes(*plen);
      if (!pdata) return Error{ErrorCode::kInvalidArgument, "ClientCellRpc: payload truncated"};
      msg.payload.assign(pdata->begin(), pdata->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<ClientCellRpc>);

// CellAppDeath (CellAppMgr → BaseApp, ID 2026). dead_addr + Space→new-host
// map. BaseApps re-issue affected entities via CreateCellEntity with
// script_init_data = cached cell_backup_data.
struct CellAppDeath {
  Address dead_addr;
  std::vector<std::pair<SpaceID, Address>> rehomes;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellAppDeath),
                                   "baseapp::CellAppDeath",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(dead_addr.Ip());
    w.Write(dead_addr.Port());
    w.WritePackedInt(static_cast<uint32_t>(rehomes.size()));
    for (const auto& [sid, addr] : rehomes) {
      w.Write(sid);
      w.Write(addr.Ip());
      w.Write(addr.Port());
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<CellAppDeath> {
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto count = r.ReadPackedInt();
    if (!ip || !port || !count)
      return Error{ErrorCode::kInvalidArgument, "CellAppDeath: truncated header"};
    CellAppDeath msg;
    msg.dead_addr = Address(*ip, *port);
    msg.rehomes.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto sid = r.Read<uint32_t>();
      auto hip = r.Read<uint32_t>();
      auto hport = r.Read<uint16_t>();
      if (!sid || !hip || !hport)
        return Error{ErrorCode::kInvalidArgument, "CellAppDeath: rehome entry truncated"};
      msg.rehomes.emplace_back(*sid, Address(*hip, *hport));
    }
    return msg;
  }
};
static_assert(NetworkMessage<CellAppDeath>);

// ClientEventSeqReport (Client → BaseApp, ID 2027); accumulated reliable-
// delta gap count since last report.
struct ClientEventSeqReport {
  EntityID base_entity_id{kInvalidEntityID};
  uint32_t gap_delta{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kClientEventSeqReport),
                                   "baseapp::ClientEventSeqReport",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint32_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(base_entity_id);
    w.Write(gap_delta);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ClientEventSeqReport> {
    auto eid = r.Read<uint32_t>();
    auto gap = r.Read<uint32_t>();
    if (!eid || !gap) return Error{ErrorCode::kInvalidArgument, "ClientEventSeqReport: truncated"};
    ClientEventSeqReport msg;
    msg.base_entity_id = *eid;
    msg.gap_delta = *gap;
    return msg;
  }
};
static_assert(NetworkMessage<ClientEventSeqReport>);

// ForceLogoff (BaseApp → BaseApp, ID 2030); evicts holder so a new login
// can check out the entity.
struct ForceLogoff {
  DatabaseID dbid{kInvalidDBID};
  uint32_t request_id{0};  // echoed back in ForceLogoffAck

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kForceLogoff),
                                   "baseapp::ForceLogoff",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(int64_t) + sizeof(uint32_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(dbid);
    w.Write(request_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ForceLogoff> {
    auto db = r.Read<int64_t>();
    auto rid = r.Read<uint32_t>();
    if (!db || !rid) return Error{ErrorCode::kInvalidArgument, "ForceLogoff: truncated"};
    ForceLogoff msg;
    msg.dbid = *db;
    msg.request_id = *rid;
    return msg;
  }
};
static_assert(NetworkMessage<ForceLogoff>);

// ForceLogoffAck (BaseApp → BaseApp, ID 2031).
struct ForceLogoffAck {
  uint32_t request_id{0};
  bool success{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kForceLogoffAck),
                                   "baseapp::ForceLogoffAck",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
  }

  static auto Deserialize(BinaryReader& r) -> Result<ForceLogoffAck> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    if (!rid || !ok) return Error{ErrorCode::kInvalidArgument, "ForceLogoffAck: truncated"};
    ForceLogoffAck msg;
    msg.request_id = *rid;
    msg.success = (*ok != 0);
    return msg;
  }
};
static_assert(NetworkMessage<ForceLogoffAck>);

// Client-facing reserved wire IDs (0xF000 range); client routes via
// SetDefaultHandler.
inline constexpr MessageID kClientDeltaMessageId = static_cast<MessageID>(0xF001);
inline constexpr MessageID kClientBaselineMessageId = static_cast<MessageID>(0xF002);
inline constexpr MessageID kClientReliableDeltaMessageId = static_cast<MessageID>(0xF003);
inline constexpr MessageID kClientRpcMessageId = static_cast<MessageID>(0xF004);

// Send-only envelopes; span borrows caller storage for the synchronous
// SendMessage. Client intercepts these wire ids before typed dispatch.

struct ClientDeltaEnvelope {
  std::span<const std::byte> bytes;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{kClientDeltaMessageId,           "baseapp::ClientDeltaEnvelope",
                                   MessageLengthStyle::kVariable,   -1,
                                   MessageReliability::kUnreliable, MessageUrgency::kBatched};
    return kDesc;
  }
  void Serialize(BinaryWriter& w) const { w.WriteBytes(bytes); }
  static auto Deserialize(BinaryReader&) -> Result<ClientDeltaEnvelope> {
    return Error{ErrorCode::kInvalidArgument, "ClientDeltaEnvelope is send-only"};
  }
};
static_assert(NetworkMessage<ClientDeltaEnvelope>);

struct ClientReliableDeltaEnvelope {
  std::span<const std::byte> bytes;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        kClientReliableDeltaMessageId, "baseapp::ClientReliableDeltaEnvelope",
        MessageLengthStyle::kVariable, -1,
        MessageReliability::kReliable, MessageUrgency::kBatched};
    return kDesc;
  }
  void Serialize(BinaryWriter& w) const { w.WriteBytes(bytes); }
  static auto Deserialize(BinaryReader&) -> Result<ClientReliableDeltaEnvelope> {
    return Error{ErrorCode::kInvalidArgument, "ClientReliableDeltaEnvelope is send-only"};
  }
};
static_assert(NetworkMessage<ClientReliableDeltaEnvelope>);

// Body: [u32 rpc_id][args].  rpc_id = slot:8 | method:24.
struct ClientRpcEnvelope {
  uint32_t rpc_id{0};
  std::span<const std::byte> args;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{kClientRpcMessageId,           "baseapp::ClientRpcEnvelope",
                                   MessageLengthStyle::kVariable, -1,
                                   MessageReliability::kReliable, MessageUrgency::kImmediate};
    return kDesc;
  }
  void Serialize(BinaryWriter& w) const {
    w.Write(rpc_id);
    w.WriteBytes(args);
  }
  static auto Deserialize(BinaryReader&) -> Result<ClientRpcEnvelope> {
    return Error{ErrorCode::kInvalidArgument, "ClientRpcEnvelope is send-only"};
  }
};
static_assert(NetworkMessage<ClientRpcEnvelope>);

}  // namespace atlas::baseapp

#endif  // ATLAS_SERVER_BASEAPP_BASEAPP_MESSAGES_H_
