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

namespace atlas::baseapp {

// ============================================================================
// CreateBase  (BaseAppMgr → BaseApp, ID 2000)
// ============================================================================

struct CreateBase {
  uint16_t type_id{0};
  EntityID entity_id{kInvalidEntityID};  // 0 = BaseApp allocates locally

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCreateBase), "baseapp::CreateBase",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint16_t) + sizeof(uint32_t))};
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

// ============================================================================
// CreateBaseFromDB  (BaseAppMgr → BaseApp, ID 2001)
// ============================================================================

struct CreateBaseFromDB {
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};
  std::string identifier;  // non-empty → load by name

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCreateBaseFromDb),
                                   "baseapp::CreateBaseFromDB", MessageLengthStyle::kVariable, -1};
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

// ============================================================================
// AcceptClient  (BaseApp → BaseApp, remote give_client_to, ID 2002)
// ============================================================================

struct AcceptClient {
  EntityID dest_entity_id{kInvalidEntityID};
  SessionKey session_key;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kAcceptClient),
                                   "baseapp::AcceptClient", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(SessionKey))};
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

// ============================================================================
// CellEntityCreated  (CellApp → BaseApp, ID 2010)
// ============================================================================

struct CellEntityCreated {
  EntityID base_entity_id{kInvalidEntityID};
  EntityID cell_entity_id{kInvalidEntityID};
  Address cell_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseApp::kCellEntityCreated), "baseapp::CellEntityCreated",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) * 2 + sizeof(uint32_t) + sizeof(uint16_t))};
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

// ============================================================================
// CellEntityDestroyed  (CellApp → BaseApp, ID 2011)
// ============================================================================

struct CellEntityDestroyed {
  EntityID base_entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellEntityDestroyed),
                                   "baseapp::CellEntityDestroyed", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t))};
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

// ============================================================================
// CurrentCell  (CellApp → BaseApp, cell address updated after offload, ID 2012)
// ============================================================================

struct CurrentCell {
  EntityID base_entity_id{kInvalidEntityID};
  EntityID cell_entity_id{kInvalidEntityID};
  Address cell_addr;
  uint32_t epoch{0};  // Monotonic; BaseApp rejects stale updates (epoch < stored).

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseApp::kCurrentCell), "baseapp::CurrentCell",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) * 3 + sizeof(uint32_t) + sizeof(uint16_t))};
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

// ============================================================================
// CellRpcForward  (CellApp → BaseApp, Cell→Base RPC, ID 2013)
// ============================================================================

struct CellRpcForward {
  EntityID base_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellRpcForward),
                                   "baseapp::CellRpcForward", MessageLengthStyle::kVariable, -1};
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

// ============================================================================
// SelfRpcFromCell  (CellApp → BaseApp → owning client, ID 2014)
//
// RPC directed at the entity's own client (target == self).
// Must be delivered reliably — loss causes permanent client state desync
// (e.g. skill hit confirmation, damage numbers, state transitions).
// ============================================================================

struct SelfRpcFromCell {
  EntityID base_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kSelfRpcFromCell),
                                   "baseapp::SelfRpcFromCell", MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(rpc_id);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    w.WriteBytes(payload);
  }

  static auto Deserialize(BinaryReader& r) -> Result<SelfRpcFromCell> {
    auto eid = r.ReadPackedInt();
    auto rid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !rid || !sz)
      return Error{ErrorCode::kInvalidArgument, "SelfRpcFromCell: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span) return Error{ErrorCode::kInvalidArgument, "SelfRpcFromCell: payload truncated"};
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

struct BroadcastRpcFromCell {
  EntityID base_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  uint8_t target{1};  // 1=otherClients, 2=allClients
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kBroadcastRpcFromCell),
                                   "baseapp::BroadcastRpcFromCell", MessageLengthStyle::kVariable,
                                   -1, MessageReliability::kUnreliable};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(base_entity_id);
    w.WritePackedInt(rpc_id);
    w.Write(target);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    w.WriteBytes(payload);
  }

  static auto Deserialize(BinaryReader& r) -> Result<BroadcastRpcFromCell> {
    auto eid = r.ReadPackedInt();
    auto rid = r.ReadPackedInt();
    auto tgt = r.Read<uint8_t>();
    auto sz = r.ReadPackedInt();
    if (!eid || !rid || !tgt || !sz)
      return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span) return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: payload truncated"};
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

struct ReplicatedDeltaFromCell {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> delta;

  // AoI / property replication deltas are superseded by the next tick —
  // best-effort delivery is preferred over head-of-line blocking.
  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseApp::kReplicatedDeltaFromCell), "baseapp::ReplicatedDeltaFromCell",
        MessageLengthStyle::kVariable, -1, MessageReliability::kUnreliable};
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

// ============================================================================
// ReplicatedReliableDeltaFromCell  (CellApp → BaseApp → Client, ID 2017)
//
// Reliable twin of ReplicatedDeltaFromCell. Carries delta bytes for properties
// marked reliable="true" in .def — fields where a dropped UDP packet cannot be
// tolerated (HP, state transitions, inventory). Bypasses the DeltaForwarder
// byte budget on the BaseApp→Client hop; reliable deltas must not be dropped.
// The payload format is identical to the unreliable variant (flags + values),
// so the client reuses ApplyReplicatedDelta for both.
// ============================================================================

struct ReplicatedReliableDeltaFromCell {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> delta;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kReplicatedReliableDeltaFromCell),
                                   "baseapp::ReplicatedReliableDeltaFromCell",
                                   MessageLengthStyle::kVariable, -1,
                                   MessageReliability::kReliable};
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

// ============================================================================
// BackupCellEntity  (CellApp → BaseApp, ID 2018)
//
// BigWorld-style cell-to-base state backup. The CellApp emits this
// periodically (every `CellApp::kBackupIntervalTicks`) for every entity
// with a BaseApp binding. `cell_backup_data` is the opaque output of the
// cell-side `ServerEntity.Serialize` — i.e. post-M2, only the CELL_DATA
// subset of properties.
//
// BaseApp stores the bytes verbatim on its Proxy (`cell_backup_data_`)
// without deserialising. That blob is the authoritative mirror of the
// cell-authoritative state for:
//   * DB writes   (base has the full persistent record: its own
//                  base-scope entity_data_ + the cell_backup_data_ blob)
//   * Reviver     (restore the entity on a new CellApp after a crash)
//   * Offload     (migration from one CellApp to another — the new cell
//                  can bootstrap from the blob instead of waiting for
//                  cross-cell ghost traffic)
//
// Mirrors BigWorld BaseAppIntInterface::backupCellEntity (bigworld/
// server/cellapp/real_entity.cpp:884-906 for the sender;
// bigworld/server/baseapp/base.cpp:1182-1200 for the "stash as opaque
// bytes" receiver).
// ============================================================================

struct BackupCellEntity {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> cell_backup_data;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kBackupCellEntity),
                                   "baseapp::BackupCellEntity", MessageLengthStyle::kVariable, -1,
                                   MessageReliability::kReliable};
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

// ============================================================================
// ReplicatedBaselineFromCell  (CellApp → BaseApp, ID 2019)
//
// L4: cell-authoritative owner snapshot, periodic. Cell has the live
// cell-scope property values; BaseApp's own SerializeForOwnerClient
// (post-M2) returns empty blobs for pure cell-scope entities because
// the base-side partial has no backing field for those props. So the
// baseline must originate where the data lives — exactly BigWorld's
// stance of "cell-auth state stays on the cell". BaseApp's only job
// is relay: on receipt it calls ResolveClientChannel(base_entity_id)
// and forwards the blob as a ReplicatedBaselineToClient (0xF002).
//
// Payload format is the raw bytes of the cell-side
// SerializeForOwnerClient — byte-identical to the pre-M2 baseapp-
// sourced baseline, so no client-side decoder change.
// ============================================================================

struct ReplicatedBaselineFromCell {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> snapshot;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kReplicatedBaselineFromCell),
                                   "baseapp::ReplicatedBaselineFromCell",
                                   MessageLengthStyle::kVariable, -1,
                                   MessageReliability::kReliable};
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

// ============================================================================
// ReplicatedBaselineToClient  (BaseApp → Client, client-facing ID 0xF002)
//
// Periodic full-state snapshot for an owning client's entity. Sent reliably
// every `BaseApp::kBaselineInterval` ticks so that any property change lost on
// the unreliable delta path is recovered within one baseline interval. The
// payload is the owner-scope serialization produced by the source generator
// (`SerializeForOwnerClient`), so the client reuses the same property reader
// it would use for deltas — only the message ID differs.
//
// This is a client-facing message (no CellApp→BaseApp hop); BaseApp itself
// assembles and sends it, so it does not appear in the BaseApp internal
// enum range (2000-2031). The struct is defined here purely for typed
// Serialize/Deserialize round-trip testing.
// ============================================================================

struct ReplicatedBaselineToClient {
  EntityID base_entity_id{kInvalidEntityID};
  std::vector<std::byte> snapshot;

  static auto Descriptor() -> const MessageDesc& {
    // Reliability MUST be reliable — that is the entire point of baseline.
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

// ============================================================================
// Authenticate  (Client → BaseApp external interface, ID 2020)
// First message sent by client after connecting to BaseApp.
// ============================================================================

struct Authenticate {
  SessionKey session_key;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kAuthenticate),
                                   "baseapp::Authenticate", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(SessionKey))};
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

// ============================================================================
// AuthenticateResult  (BaseApp → Client, ID 2021)
// ============================================================================

struct AuthenticateResult {
  bool success{false};
  EntityID entity_id{kInvalidEntityID};
  uint16_t type_id{0};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kAuthenticateResult),
                                   "baseapp::AuthenticateResult", MessageLengthStyle::kVariable,
                                   -1};
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

// ============================================================================
// EntityTransferred  (BaseApp → Client, ID 2024)
//
// Notifies the client that its owning entity has changed as a result of a
// server-side GiveClientTo handoff (e.g. Account → Avatar at SelectAvatar).
// After this arrives, the client must use `new_entity_id` as the
// `target_entity_id` for subsequent ClientCellRpc messages; the previous
// id is unbound from the RUDP channel and will be rejected at validation.
// `new_type_id` lets the client identify which entity class is now active
// (useful when the client needs to construct type-specific RPC ids).
// ============================================================================

struct EntityTransferred {
  EntityID new_entity_id{kInvalidEntityID};
  uint16_t new_type_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kEntityTransferred),
                                   "baseapp::EntityTransferred", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t))};
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

// ============================================================================
// CellReady  (BaseApp → Client, ID 2025)
//
// Fired when BaseApp records the cell_addr on a client-bound Proxy, i.e.
// after CellEntityCreated arrives from the CellApp. Eliminates the race
// where the client would try to ClientCellRpc before cell_addr was set
// and see BaseApp's "no cell channel for target entity" drop.
//
// Emission: once per entity, at the first moment both
//   (1) the entity has a cell (SetCell has fired), AND
//   (2) the entity has a client (BindClient has fired)
// are simultaneously true. In practice OnCellEntityCreated is the
// second of those two edges on the world_stress script flow because
// GiveClientTo runs synchronously before the CellApp ack can arrive.
// ============================================================================

struct CellReady {
  EntityID entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellReady), "baseapp::CellReady",
                                   MessageLengthStyle::kFixed, static_cast<int>(sizeof(uint32_t))};
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

// ============================================================================
// ClientBaseRpc  (Client → BaseApp external interface, ID 2022)
// Client sends an exposed base method call. BaseApp validates the exposed scope
// and dispatches to the C# entity via the dispatch_rpc callback.
// ============================================================================

struct ClientBaseRpc {
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kClientBaseRpc),
                                   "baseapp::ClientBaseRpc", MessageLengthStyle::kVariable, -1};
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

// ============================================================================
// ClientCellRpc  (Client → BaseApp external interface, ID 2023)
//
// Client sends an exposed cell method call. BaseApp validates direction/
// exposed/cross-entity rules, embeds `source_entity_id = proxy.entity_id()`
// (un-spoofable), and forwards to the owning CellApp as ClientCellRpcForward.
//
// Wire format: [target_entity_id | rpc_id | payload].
// `target_entity_id` is the base_entity_id space — clients only ever know
// base ids; CellApp's base_entity_population_ index resolves it to the
// CellEntity on arrival.
//
// Phase 10 prerequisite PR-A: struct + handler skeleton. The full validation
// chain (direction==0x02, IsExposed, cross-entity + AllClients) is wired in
// Step 10.9 once the registry direction/exposed metadata is guaranteed.
// ============================================================================

struct ClientCellRpc {
  EntityID target_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kClientCellRpc),
                                   "baseapp::ClientCellRpc", MessageLengthStyle::kVariable, -1};
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

// ============================================================================
// CellAppDeath  (CellAppMgr → BaseApp, ID 2026)
//
// CellAppMgr fans this out to every BaseApp after it rehomes the dead
// CellApp's BSP leaves. `dead_addr` identifies which CellApp died;
// `rehomes` maps each affected Space to the surviving CellApp that now
// owns a replacement Cell. BaseApps walk their entities: every
// BaseEntity with `cell_addr == dead_addr` gets re-issued to the
// corresponding new host via CreateCellEntity (script_init_data =
// last cached cell_backup_data), which rehydrates the Real from the
// base-side backup. BigWorld parity:
// `BaseApp::handleCellAppDeath` + `DyingCellApp::tick`
// (dead_cell_apps.cpp:234).
// ============================================================================

struct CellAppDeath {
  Address dead_addr;
  std::vector<std::pair<SpaceID, Address>> rehomes;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellAppDeath),
                                   "baseapp::CellAppDeath", MessageLengthStyle::kVariable, -1};
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

// ============================================================================
// ForceLogoff  (BaseApp → BaseApp, ID 2030)
// Sent to evict an existing Proxy so the new login can checkout the entity.
// ============================================================================

struct ForceLogoff {
  DatabaseID dbid{kInvalidDBID};
  uint32_t request_id{0};  // echoed back in ForceLogoffAck

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kForceLogoff),
                                   "baseapp::ForceLogoff", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(int64_t) + sizeof(uint32_t))};
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

// ============================================================================
// ForceLogoffAck  (BaseApp → BaseApp, ID 2031)
// ============================================================================

struct ForceLogoffAck {
  uint32_t request_id{0};
  bool success{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kForceLogoffAck),
                                   "baseapp::ForceLogoffAck", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t))};
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

}  // namespace atlas::baseapp

#endif  // ATLAS_SERVER_BASEAPP_BASEAPP_MESSAGES_H_
