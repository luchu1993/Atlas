#ifndef ATLAS_SERVER_BASEAPP_BASEAPP_MESSAGES_H_
#define ATLAS_SERVER_BASEAPP_BASEAPP_MESSAGES_H_

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "cellapp/cell_bounds.h"
#include "math/vector3.h"
#include "movement_sim/movement_codec.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "server/entity_types.h"

namespace atlas::baseapp {

inline constexpr uint32_t kMaxBroadcastRpcDestinations = 64 * 1024;
inline constexpr uint32_t kMaxCellAppDeathRehomes = 64 * 1024;
inline constexpr uint32_t kMaxCellAppDeathRehomeCells = 64 * 1024;

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

struct CreateBaseFromDB {
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};
  std::string identifier;  // non-empty -> load by name

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

struct CellEntityCreated {
  EntityID entity_id{kInvalidEntityID};
  Address cell_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseApp::kCellEntityCreated),
        "baseapp::CellEntityCreated",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(cell_addr.Ip());
    w.Write(cell_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<CellEntityCreated> {
    auto eid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    if (!eid || !ip || !port)
      return Error{ErrorCode::kInvalidArgument, "CellEntityCreated: truncated"};
    CellEntityCreated msg;
    msg.entity_id = *eid;
    msg.cell_addr = Address(*ip, *port);
    return msg;
  }
};
static_assert(NetworkMessage<CellEntityCreated>);

enum class CellEntityCreateFailureReason : uint8_t {
  kRejected = 0,
  kInvalidSpace = 1,
  kInvalidEntity = 2,
  kExistingMismatch = 3,
  kGhostRequiredMissing = 4,
  kGhostBackupMissing = 5,
};

struct CellEntityCreateFailed {
  EntityID entity_id{kInvalidEntityID};
  uint32_t request_id{0};
  CellEntityCreateFailureReason reason{CellEntityCreateFailureReason::kRejected};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseApp::kCellEntityCreateFailed),
        "baseapp::CellEntityCreateFailed",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(reason));
  }

  static auto Deserialize(BinaryReader& r) -> Result<CellEntityCreateFailed> {
    auto eid = r.Read<uint32_t>();
    auto rid = r.Read<uint32_t>();
    auto rsn = r.Read<uint8_t>();
    if (!eid || !rid || !rsn)
      return Error{ErrorCode::kInvalidArgument, "CellEntityCreateFailed: truncated"};
    CellEntityCreateFailed msg;
    msg.entity_id = *eid;
    msg.request_id = *rid;
    msg.reason = static_cast<CellEntityCreateFailureReason>(*rsn);
    return msg;
  }
};
static_assert(NetworkMessage<CellEntityCreateFailed>);

struct CellEntityDestroyed {
  EntityID entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kCellEntityDestroyed),
                                   "baseapp::CellEntityDestroyed",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(entity_id); }

  static auto Deserialize(BinaryReader& r) -> Result<CellEntityDestroyed> {
    auto eid = r.Read<uint32_t>();
    if (!eid) return Error{ErrorCode::kInvalidArgument, "CellEntityDestroyed: truncated"};
    CellEntityDestroyed msg;
    msg.entity_id = *eid;
    return msg;
  }
};
static_assert(NetworkMessage<CellEntityDestroyed>);

struct CurrentCell {
  EntityID entity_id{kInvalidEntityID};
  Address cell_addr;
  // Per-entity migration counter; lives with the entity and bumps on every
  // Offload accept so order is monotonic regardless of which cellapp emits.
  uint32_t epoch{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseApp::kCurrentCell),
        "baseapp::CurrentCell",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) * 2 + sizeof(uint32_t) + sizeof(uint16_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(cell_addr.Ip());
    w.Write(cell_addr.Port());
    w.Write(epoch);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CurrentCell> {
    auto eid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto ep = r.Read<uint32_t>();
    if (!eid || !ip || !port || !ep)
      return Error{ErrorCode::kInvalidArgument, "CurrentCell: truncated"};
    CurrentCell msg;
    msg.entity_id = *eid;
    msg.cell_addr = Address(*ip, *port);
    msg.epoch = *ep;
    return msg;
  }
};
static_assert(NetworkMessage<CurrentCell>);

struct CellRpcForward {
  EntityID entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;
  uint64_t trace_id{0};

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
    w.WritePackedInt(entity_id);
    w.WritePackedInt(rpc_id);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    w.WriteBytes(payload);
    w.Write(trace_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CellRpcForward> {
    auto eid = r.ReadPackedInt();
    auto rid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !rid || !sz) return Error{ErrorCode::kInvalidArgument, "CellRpcForward: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span) return Error{ErrorCode::kInvalidArgument, "CellRpcForward: payload truncated"};
    auto tid = r.Read<uint64_t>();
    if (!tid) return Error{ErrorCode::kInvalidArgument, "CellRpcForward: trace_id truncated"};
    CellRpcForward msg;
    msg.entity_id = *eid;
    msg.rpc_id = *rid;
    msg.payload.assign(span->begin(), span->end());
    msg.trace_id = *tid;
    return msg;
  }
};
static_assert(NetworkMessage<CellRpcForward>);

// Cell-scope-resolved RPC fan-out (ID 2016); BaseApp dispatches to each
// destination's bound client via the unified RPC envelope.
struct BroadcastRpcFromCell {
  uint32_t rpc_id{0};
  EntityID source_entity_id{kInvalidEntityID};
  std::vector<EntityID> dest_entity_ids;
  std::vector<std::byte> payload;
  uint64_t trace_id{0};

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
    w.WritePackedInt(source_entity_id);
    w.WritePackedInt(static_cast<uint32_t>(dest_entity_ids.size()));
    for (auto id : dest_entity_ids) w.WritePackedInt(id);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    w.WriteBytes(payload);
    w.Write(trace_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<BroadcastRpcFromCell> {
    auto rid = r.ReadPackedInt();
    if (!rid) return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: truncated rpc_id"};
    auto src = r.ReadPackedInt();
    if (!src)
      return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: truncated source_entity_id"};
    auto count = r.ReadPackedInt();
    if (!count)
      return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: truncated dest count"};
    if (*count > kMaxBroadcastRpcDestinations) {
      return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: too many destinations"};
    }
    BroadcastRpcFromCell msg;
    msg.rpc_id = *rid;
    msg.source_entity_id = *src;
    msg.dest_entity_ids.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto id = r.ReadPackedInt();
      if (!id)
        return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: dest list truncated"};
      msg.dest_entity_ids.push_back(*id);
    }
    auto sz = r.ReadPackedInt();
    if (!sz)
      return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: missing payload size"};
    auto span = r.ReadBytes(*sz);
    if (!span) return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: payload truncated"};
    msg.payload.assign(span->begin(), span->end());
    auto tid = r.Read<uint64_t>();
    if (!tid) return Error{ErrorCode::kInvalidArgument, "BroadcastRpcFromCell: trace_id truncated"};
    msg.trace_id = *tid;
    return msg;
  }
};
static_assert(NetworkMessage<BroadcastRpcFromCell>);

// Unreliable: next tick supersedes; HoL blocking is worse than loss.
struct ReplicatedDeltaFromCell {
  EntityID entity_id{kInvalidEntityID};
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
    w.WritePackedInt(entity_id);
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
    msg.entity_id = *eid;
    msg.delta.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<ReplicatedDeltaFromCell>);

// Send-only zero-copy view of ReplicatedDeltaFromCell. Saves ~3 us/observer
// at 200 obs/tick by skipping per-observer std::vector::assign.
struct ReplicatedDeltaFromCellSpan {
  EntityID entity_id{kInvalidEntityID};
  std::span<const std::byte> delta;

  static auto Descriptor() -> const MessageDesc& { return ReplicatedDeltaFromCell::Descriptor(); }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(entity_id);
    w.WritePackedInt(static_cast<uint32_t>(delta.size()));
    w.WriteBytes(delta);
  }

  // Send-only stub to satisfy NetworkMessage concept.
  static auto Deserialize(BinaryReader&) -> Result<ReplicatedDeltaFromCellSpan> {
    return Error{ErrorCode::kInvalidArgument, "ReplicatedDeltaFromCellSpan is send-only"};
  }
};
static_assert(NetworkMessage<ReplicatedDeltaFromCellSpan>);

// Reliable twin of ReplicatedDeltaFromCell for ordered fields.
// It bypasses DeltaForwarder and reuses the same wire format.
struct ReplicatedReliableDeltaFromCell {
  EntityID entity_id{kInvalidEntityID};
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
    w.WritePackedInt(entity_id);
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
    msg.entity_id = *eid;
    msg.delta.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<ReplicatedReliableDeltaFromCell>);

// Send-only zero-copy view; see ReplicatedDeltaFromCellSpan.
struct ReplicatedReliableDeltaFromCellSpan {
  EntityID entity_id{kInvalidEntityID};
  std::span<const std::byte> delta;

  static auto Descriptor() -> const MessageDesc& {
    return ReplicatedReliableDeltaFromCell::Descriptor();
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(entity_id);
    w.WritePackedInt(static_cast<uint32_t>(delta.size()));
    w.WriteBytes(delta);
  }

  static auto Deserialize(BinaryReader&) -> Result<ReplicatedReliableDeltaFromCellSpan> {
    return Error{ErrorCode::kInvalidArgument, "ReplicatedReliableDeltaFromCellSpan is send-only"};
  }
};
static_assert(NetworkMessage<ReplicatedReliableDeltaFromCellSpan>);

// Periodic cell-to-base state backup (ID 2018); opaque CELL_DATA plus the
// last authoritative pose used for crash restore placement.
struct BackupCellEntity {
  EntityID entity_id{kInvalidEntityID};
  std::vector<std::byte> cell_backup_data;
  bool has_pose{false};
  math::Vector3 position{0.f, 0.f, 0.f};
  math::Vector3 direction{1.f, 0.f, 0.f};
  bool on_ground{false};

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
    w.WritePackedInt(entity_id);
    w.WritePackedInt(static_cast<uint32_t>(cell_backup_data.size()));
    w.WriteBytes(cell_backup_data);
    w.Write<uint8_t>(has_pose ? 1u : 0u);
    w.Write(position.x);
    w.Write(position.y);
    w.Write(position.z);
    w.Write(direction.x);
    w.Write(direction.y);
    w.Write(direction.z);
    w.Write<uint8_t>(on_ground ? 1u : 0u);
  }

  static auto Deserialize(BinaryReader& r) -> Result<BackupCellEntity> {
    auto eid = r.ReadPackedInt();
    auto sz = r.ReadPackedInt();
    if (!eid || !sz) return Error{ErrorCode::kInvalidArgument, "BackupCellEntity: truncated"};
    auto span = r.ReadBytes(*sz);
    if (!span) return Error{ErrorCode::kInvalidArgument, "BackupCellEntity: blob truncated"};
    BackupCellEntity msg;
    msg.entity_id = *eid;
    msg.cell_backup_data.assign(span->begin(), span->end());
    if (r.Remaining() == 0) return msg;
    auto hp = r.Read<uint8_t>();
    auto px = r.Read<float>();
    auto py = r.Read<float>();
    auto pz = r.Read<float>();
    auto dx = r.Read<float>();
    auto dy = r.Read<float>();
    auto dz = r.Read<float>();
    auto og = r.Read<uint8_t>();
    if (!hp || !px || !py || !pz || !dx || !dy || !dz || !og) {
      return Error{ErrorCode::kInvalidArgument, "BackupCellEntity: pose truncated"};
    }
    msg.has_pose = *hp != 0;
    msg.position = {*px, *py, *pz};
    msg.direction = {*dx, *dy, *dz};
    msg.on_ground = *og != 0;
    return msg;
  }
};
static_assert(NetworkMessage<BackupCellEntity>);

// Periodic cell-authoritative owner snapshot (ID 2019). BaseApp relays as
// ReplicatedBaselineToClient (0xF002).
struct ReplicatedBaselineFromCell {
  EntityID entity_id{kInvalidEntityID};
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
    w.WritePackedInt(entity_id);
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
    msg.entity_id = *eid;
    msg.snapshot.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<ReplicatedBaselineFromCell>);

// Periodic owner-snapshot to client (client-facing ID 0xF002). Reliable;
// refreshes owner-scope state after AoI entry, migration, or app-layer gaps.
struct ReplicatedBaselineToClient {
  EntityID entity_id{kInvalidEntityID};
  std::vector<std::byte> snapshot;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        static_cast<MessageID>(0xF002), "baseapp::ReplicatedBaselineToClient",
        MessageLengthStyle::kVariable, -1, MessageReliability::kReliable};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(entity_id);
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
    msg.entity_id = *eid;
    msg.snapshot.assign(span->begin(), span->end());
    return msg;
  }
};
static_assert(NetworkMessage<ReplicatedBaselineToClient>);

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

// Owning-entity change after server-side GiveClientTo (ID 2024).
// ClientCellRpc must use new_entity_id after this.
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

// Tells client its cell is ready after BindClient and SetCell.
// Avoids ClientCellRpc being dropped for missing cell_addr.
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

struct ClientBaseRpc {
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;
  uint64_t trace_id{0};

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
    w.Write(trace_id);
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
    auto tid = r.Read<uint64_t>();
    if (!tid) return Error{ErrorCode::kInvalidArgument, "ClientBaseRpc: trace_id truncated"};
    msg.trace_id = *tid;
    return msg;
  }
};
static_assert(NetworkMessage<ClientBaseRpc>);

// then forwards to CellApp with un-spoofable source_entity_id stamped.
struct ClientCellRpc {
  EntityID target_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;
  uint64_t trace_id{0};

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
    w.Write(trace_id);
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
    auto trace = r.Read<uint64_t>();
    if (!trace) return Error{ErrorCode::kInvalidArgument, "ClientCellRpc: trace_id truncated"};
    msg.trace_id = *trace;
    return msg;
  }
};
static_assert(NetworkMessage<ClientCellRpc>);

struct ClientMovementInput {
  EntityID target_entity_id{kInvalidEntityID};
  std::vector<movement::InputFrame> frames;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kClientMovementInput),
                                   "baseapp::ClientMovementInput",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kUnreliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(target_entity_id);
    w.Write(static_cast<uint8_t>(frames.size()));
    for (const auto& frame : frames) movement::SerializeInputFrame(w, frame);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ClientMovementInput> {
    auto target = r.Read<uint32_t>();
    auto count = r.Read<uint8_t>();
    if (!target || !count) {
      return Error{ErrorCode::kInvalidArgument, "ClientMovementInput: truncated"};
    }
    if (*count == 0 || *count > movement::kMaxMovementInputFrames) {
      return Error{ErrorCode::kInvalidArgument, "ClientMovementInput: invalid frame count"};
    }
    if (*target == kInvalidEntityID) {
      return Error{ErrorCode::kInvalidArgument, "ClientMovementInput: invalid target"};
    }
    ClientMovementInput msg;
    msg.target_entity_id = *target;
    msg.frames.reserve(*count);
    for (uint8_t i = 0; i < *count; ++i) {
      auto frame = movement::DeserializeInputFrame(r);
      if (!frame) {
        return Error{ErrorCode::kInvalidArgument, "ClientMovementInput: frame truncated"};
      }
      if (!movement::IsInputFrameValid(*frame)) {
        return Error{ErrorCode::kInvalidArgument, "ClientMovementInput: invalid frame"};
      }
      msg.frames.push_back(*frame);
    }
    return msg;
  }
};
static_assert(NetworkMessage<ClientMovementInput>);

struct MovementCorrectionReport {
  EntityID target_entity_id{kInvalidEntityID};
  uint32_t acked_input_seq{0};
  uint32_t server_tick{0};
  float distance_m{0.0f};
  uint16_t correction_flags{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kMovementCorrectionReport),
                                   "baseapp::MovementCorrectionReport",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) * 3 + sizeof(float) +
                                                    sizeof(uint16_t)),
                                   MessageReliability::kUnreliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(target_entity_id);
    w.Write(acked_input_seq);
    w.Write(server_tick);
    w.Write(distance_m);
    w.Write(correction_flags);
  }

  static auto Deserialize(BinaryReader& r) -> Result<MovementCorrectionReport> {
    auto target = r.Read<uint32_t>();
    auto ack = r.Read<uint32_t>();
    auto tick = r.Read<uint32_t>();
    auto distance = r.Read<float>();
    auto flags = r.Read<uint16_t>();
    if (!target || !ack || !tick || !distance || !flags) {
      return Error{ErrorCode::kInvalidArgument, "MovementCorrectionReport: truncated"};
    }
    if (*target == kInvalidEntityID) {
      return Error{ErrorCode::kInvalidArgument, "MovementCorrectionReport: invalid target"};
    }
    if (!std::isfinite(*distance) || *distance < 0.0f ||
        !movement::IsCorrectionFlagsValid(*flags)) {
      return Error{ErrorCode::kInvalidArgument, "MovementCorrectionReport: invalid payload"};
    }
    const uint16_t expected_flags =
        movement::CorrectionFlagForTier(movement::ClassifyCorrection(*distance));
    if (*flags != expected_flags) {
      return Error{ErrorCode::kInvalidArgument, "MovementCorrectionReport: mismatched flags"};
    }
    MovementCorrectionReport msg;
    msg.target_entity_id = *target;
    msg.acked_input_seq = *ack;
    msg.server_tick = *tick;
    msg.distance_m = *distance;
    msg.correction_flags = *flags;
    return msg;
  }
};
static_assert(NetworkMessage<MovementCorrectionReport>);

// CellAppMgr death notice with per-space fallback hosts plus current leaf
// bounds. BaseApp uses the bounds for exact restore host selection.
struct CellAppDeath {
  struct RehomeCell {
    SpaceID space_id{kInvalidSpaceID};
    uint32_t cell_id{0};
    Address host_addr;
    CellBounds bounds;
  };

  Address dead_addr;
  std::vector<std::pair<SpaceID, Address>> rehomes;
  std::vector<RehomeCell> rehome_cells;

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
    w.WritePackedInt(static_cast<uint32_t>(rehome_cells.size()));
    for (const auto& cell : rehome_cells) {
      w.Write(cell.space_id);
      w.Write(cell.cell_id);
      w.Write(cell.host_addr.Ip());
      w.Write(cell.host_addr.Port());
      cell.bounds.Serialize(w);
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<CellAppDeath> {
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto count = r.ReadPackedInt();
    if (!ip || !port || !count)
      return Error{ErrorCode::kInvalidArgument, "CellAppDeath: truncated header"};
    if (*count > kMaxCellAppDeathRehomes) {
      return Error{ErrorCode::kInvalidArgument, "CellAppDeath: too many rehomes"};
    }
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
    if (r.Remaining() == 0) return msg;
    auto cell_count = r.ReadPackedInt();
    if (!cell_count) {
      return Error{ErrorCode::kInvalidArgument, "CellAppDeath: truncated rehome cell count"};
    }
    if (*cell_count > kMaxCellAppDeathRehomeCells) {
      return Error{ErrorCode::kInvalidArgument, "CellAppDeath: too many rehome cells"};
    }
    msg.rehome_cells.reserve(*cell_count);
    for (uint32_t i = 0; i < *cell_count; ++i) {
      auto sid = r.Read<uint32_t>();
      auto cid = r.Read<uint32_t>();
      auto hip = r.Read<uint32_t>();
      auto hport = r.Read<uint16_t>();
      auto bounds = CellBounds::Deserialize(r);
      if (!sid || !cid || !hip || !hport || !bounds) {
        return Error{ErrorCode::kInvalidArgument, "CellAppDeath: rehome cell truncated"};
      }
      msg.rehome_cells.push_back({*sid, *cid, Address(*hip, *hport), *bounds});
    }
    return msg;
  }
};
static_assert(NetworkMessage<CellAppDeath>);

[[nodiscard]] inline auto FindRehomeCellForPosition(const CellAppDeath& msg, SpaceID space_id,
                                                    const math::Vector3& position)
    -> const CellAppDeath::RehomeCell* {
  for (const auto& cell : msg.rehome_cells) {
    if (cell.space_id == space_id && cell.bounds.Contains(position.x, position.z)) return &cell;
  }
  return nullptr;
}

// delta gap count since last report.
struct ClientEventSeqReport {
  EntityID entity_id{kInvalidEntityID};
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
    w.Write(entity_id);
    w.Write(gap_delta);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ClientEventSeqReport> {
    auto eid = r.Read<uint32_t>();
    auto gap = r.Read<uint32_t>();
    if (!eid || !gap) return Error{ErrorCode::kInvalidArgument, "ClientEventSeqReport: truncated"};
    ClientEventSeqReport msg;
    msg.entity_id = *eid;
    msg.gap_delta = *gap;
    return msg;
  }
};
static_assert(NetworkMessage<ClientEventSeqReport>);

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
inline constexpr MessageID kClientMovementStateAckMessageId = static_cast<MessageID>(0xF005);
inline constexpr MessageID kClientMovementCommandStartMessageId =
    static_cast<MessageID>(0xF006);
inline constexpr MessageID kClientMovementCommandEndMessageId =
    static_cast<MessageID>(0xF007);

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

// Body: [u32 entity_id][u32 rpc_id][u64 trace_id][args].
// entity_id targets the script entity; broadcast scopes route to the source.
struct ClientRpcEnvelope {
  EntityID entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  uint64_t trace_id{0};
  std::span<const std::byte> args;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{kClientRpcMessageId,           "baseapp::ClientRpcEnvelope",
                                   MessageLengthStyle::kVariable, -1,
                                   MessageReliability::kReliable, MessageUrgency::kImmediate};
    return kDesc;
  }
  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(rpc_id);
    w.Write(trace_id);
    w.WriteBytes(args);
  }
  static auto Deserialize(BinaryReader&) -> Result<ClientRpcEnvelope> {
    return Error{ErrorCode::kInvalidArgument, "ClientRpcEnvelope is send-only"};
  }
};
static_assert(NetworkMessage<ClientRpcEnvelope>);

struct MovementStateAckFromCell {
  EntityID entity_id{kInvalidEntityID};
  uint32_t acked_input_seq{0};
  uint32_t server_tick{0};
  uint32_t cell_epoch{0};
  movement::MovementState state;
  uint16_t correction_flags{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kMovementStateAckFromCell),
                                   "baseapp::MovementStateAckFromCell",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) * 6 + 9 * sizeof(float) +
                                                    sizeof(uint16_t)),
                                   MessageReliability::kUnreliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(acked_input_seq);
    w.Write(server_tick);
    w.Write(cell_epoch);
    movement::SerializeMovementState(w, state);
    w.Write(correction_flags);
  }

  static auto Deserialize(BinaryReader& r) -> Result<MovementStateAckFromCell> {
    auto eid = r.Read<uint32_t>();
    auto ack = r.Read<uint32_t>();
    auto tick = r.Read<uint32_t>();
    auto epoch = r.Read<uint32_t>();
    if (!eid || !ack || !tick || !epoch) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckFromCell: truncated"};
    }
    if (*eid == kInvalidEntityID) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckFromCell: invalid entity"};
    }
    auto state = movement::DeserializeMovementState(r);
    if (!state) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckFromCell: state truncated"};
    }
    auto flags = r.Read<uint16_t>();
    if (!flags) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckFromCell: truncated"};
    }
    if (!movement::IsCorrectionFlagsValid(*flags)) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckFromCell: invalid flags"};
    }
    MovementStateAckFromCell msg;
    msg.entity_id = *eid;
    msg.acked_input_seq = *ack;
    msg.server_tick = *tick;
    msg.cell_epoch = *epoch;
    msg.state = *state;
    msg.correction_flags = *flags;
    return msg;
  }
};
static_assert(NetworkMessage<MovementStateAckFromCell>);

struct MovementStateAckToClient {
  EntityID entity_id{kInvalidEntityID};
  uint32_t acked_input_seq{0};
  uint32_t server_tick{0};
  movement::MovementState state;
  uint16_t correction_flags{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{kClientMovementStateAckMessageId,
                                   "baseapp::MovementStateAckToClient",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) * 5 + 9 * sizeof(float) +
                                                    sizeof(uint16_t)),
                                   MessageReliability::kUnreliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(acked_input_seq);
    w.Write(server_tick);
    movement::SerializeMovementState(w, state);
    w.Write(correction_flags);
  }

  static auto Deserialize(BinaryReader& r) -> Result<MovementStateAckToClient> {
    auto eid = r.Read<uint32_t>();
    auto ack = r.Read<uint32_t>();
    auto tick = r.Read<uint32_t>();
    if (!eid || !ack || !tick) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckToClient: truncated"};
    }
    if (*eid == kInvalidEntityID) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckToClient: invalid entity"};
    }
    auto state = movement::DeserializeMovementState(r);
    if (!state) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckToClient: state truncated"};
    }
    auto flags = r.Read<uint16_t>();
    if (!flags) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckToClient: truncated"};
    }
    if (!movement::IsCorrectionFlagsValid(*flags)) {
      return Error{ErrorCode::kInvalidArgument, "MovementStateAckToClient: invalid flags"};
    }
    MovementStateAckToClient msg;
    msg.entity_id = *eid;
    msg.acked_input_seq = *ack;
    msg.server_tick = *tick;
    msg.state = *state;
    msg.correction_flags = *flags;
    return msg;
  }
};
static_assert(NetworkMessage<MovementStateAckToClient>);

struct MovementCommandStartFromCell {
  EntityID source_entity_id{kInvalidEntityID};
  uint32_t cell_epoch{0};
  std::vector<EntityID> dest_entity_ids;
  movement::MovementCommand command;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kMovementCommandStartFromCell),
                                   "baseapp::MovementCommandStartFromCell",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(source_entity_id);
    w.Write(cell_epoch);
    w.WritePackedInt(static_cast<uint32_t>(dest_entity_ids.size()));
    for (auto id : dest_entity_ids) w.WritePackedInt(id);
    movement::SerializeMovementCommand(w, command);
  }

  static auto Deserialize(BinaryReader& r) -> Result<MovementCommandStartFromCell> {
    auto source = r.ReadPackedInt();
    auto epoch = r.Read<uint32_t>();
    auto count = r.ReadPackedInt();
    if (!source || !epoch || !count) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandStartFromCell: truncated"};
    }
    if (*source == kInvalidEntityID) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandStartFromCell: invalid source"};
    }
    if (*count > kMaxBroadcastRpcDestinations) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandStartFromCell: too many dests"};
    }
    MovementCommandStartFromCell msg;
    msg.source_entity_id = *source;
    msg.cell_epoch = *epoch;
    msg.dest_entity_ids.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto id = r.ReadPackedInt();
      if (!id) {
        return Error{ErrorCode::kInvalidArgument,
                     "MovementCommandStartFromCell: dest list truncated"};
      }
      if (*id == kInvalidEntityID) {
        return Error{ErrorCode::kInvalidArgument, "MovementCommandStartFromCell: invalid dest"};
      }
      msg.dest_entity_ids.push_back(*id);
    }
    auto command = movement::DeserializeMovementCommand(r);
    if (!command) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandStartFromCell: command"};
    }
    msg.command = *command;
    return msg;
  }
};
static_assert(NetworkMessage<MovementCommandStartFromCell>);

struct MovementCommandEndFromCell {
  EntityID source_entity_id{kInvalidEntityID};
  uint32_t cell_epoch{0};
  std::vector<EntityID> dest_entity_ids;
  uint32_t command_id{0};
  uint32_t server_tick{0};
  movement::MovementCommandEndReason reason{movement::MovementCommandEndReason::kCompleted};
  movement::MovementState state;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kMovementCommandEndFromCell),
                                   "baseapp::MovementCommandEndFromCell",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(source_entity_id);
    w.Write(cell_epoch);
    w.WritePackedInt(static_cast<uint32_t>(dest_entity_ids.size()));
    for (auto id : dest_entity_ids) w.WritePackedInt(id);
    w.Write(command_id);
    w.Write(server_tick);
    w.Write(static_cast<uint8_t>(reason));
    movement::SerializeMovementState(w, state);
  }

  static auto Deserialize(BinaryReader& r) -> Result<MovementCommandEndFromCell> {
    auto source = r.ReadPackedInt();
    auto epoch = r.Read<uint32_t>();
    auto count = r.ReadPackedInt();
    if (!source || !epoch || !count) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndFromCell: truncated"};
    }
    if (*source == kInvalidEntityID) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndFromCell: invalid source"};
    }
    if (*count > kMaxBroadcastRpcDestinations) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndFromCell: too many dests"};
    }
    MovementCommandEndFromCell msg;
    msg.source_entity_id = *source;
    msg.cell_epoch = *epoch;
    msg.dest_entity_ids.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto id = r.ReadPackedInt();
      if (!id) {
        return Error{ErrorCode::kInvalidArgument,
                     "MovementCommandEndFromCell: dest list truncated"};
      }
      if (*id == kInvalidEntityID) {
        return Error{ErrorCode::kInvalidArgument, "MovementCommandEndFromCell: invalid dest"};
      }
      msg.dest_entity_ids.push_back(*id);
    }
    auto command_id = r.Read<uint32_t>();
    auto server_tick = r.Read<uint32_t>();
    auto reason = r.Read<uint8_t>();
    if (!command_id || !server_tick || !reason) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndFromCell: truncated"};
    }
    if (*command_id == 0) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndFromCell: invalid command id"};
    }
    if (!movement::IsMovementCommandEndReasonWireValue(*reason)) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndFromCell: invalid reason"};
    }
    auto state = movement::DeserializeMovementState(r);
    if (!state) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndFromCell: state"};
    }
    msg.command_id = *command_id;
    msg.server_tick = *server_tick;
    msg.reason = static_cast<movement::MovementCommandEndReason>(*reason);
    msg.state = *state;
    return msg;
  }
};
static_assert(NetworkMessage<MovementCommandEndFromCell>);

struct MovementCommandStartToClient {
  EntityID entity_id{kInvalidEntityID};
  movement::MovementCommand command;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{kClientMovementCommandStartMessageId,
                                   "baseapp::MovementCommandStartToClient",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) +
                                                    movement::kMovementCommandWireBytes),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    movement::SerializeMovementCommand(w, command);
  }

  static auto Deserialize(BinaryReader& r) -> Result<MovementCommandStartToClient> {
    auto eid = r.Read<uint32_t>();
    if (!eid) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandStartToClient: truncated"};
    }
    if (*eid == kInvalidEntityID) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandStartToClient: invalid entity"};
    }
    auto command = movement::DeserializeMovementCommand(r);
    if (!command) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandStartToClient: command"};
    }
    MovementCommandStartToClient msg;
    msg.entity_id = *eid;
    msg.command = *command;
    return msg;
  }
};
static_assert(NetworkMessage<MovementCommandStartToClient>);

struct MovementCommandEndToClient {
  EntityID entity_id{kInvalidEntityID};
  uint32_t command_id{0};
  uint32_t server_tick{0};
  movement::MovementCommandEndReason reason{movement::MovementCommandEndReason::kCompleted};
  movement::MovementState state;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{kClientMovementCommandEndMessageId,
                                   "baseapp::MovementCommandEndToClient",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) * 5 +
                                                    9 * sizeof(float) +
                                                    movement::kMovementCommandEndReasonWireBytes),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(command_id);
    w.Write(server_tick);
    w.Write(static_cast<uint8_t>(reason));
    movement::SerializeMovementState(w, state);
  }

  static auto Deserialize(BinaryReader& r) -> Result<MovementCommandEndToClient> {
    auto eid = r.Read<uint32_t>();
    auto command_id = r.Read<uint32_t>();
    auto server_tick = r.Read<uint32_t>();
    auto reason = r.Read<uint8_t>();
    if (!eid || !command_id || !server_tick || !reason) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndToClient: truncated"};
    }
    if (*eid == kInvalidEntityID) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndToClient: invalid entity"};
    }
    if (*command_id == 0) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndToClient: invalid command id"};
    }
    if (!movement::IsMovementCommandEndReasonWireValue(*reason)) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndToClient: invalid reason"};
    }
    auto state = movement::DeserializeMovementState(r);
    if (!state) {
      return Error{ErrorCode::kInvalidArgument, "MovementCommandEndToClient: state"};
    }
    MovementCommandEndToClient msg;
    msg.entity_id = *eid;
    msg.command_id = *command_id;
    msg.server_tick = *server_tick;
    msg.reason = static_cast<movement::MovementCommandEndReason>(*reason);
    msg.state = *state;
    return msg;
  }
};
static_assert(NetworkMessage<MovementCommandEndToClient>);

// Flattened BSP leaf rects for the LB debug visualisation.
// Same id is used for mgr->base and base->client forwarding.
struct SpaceBspGeometry {
  SpaceID space_id{kInvalidSpaceID};
  struct LeafRect {
    uint32_t cell_id{0};
    uint8_t owner_index{0};  // stable per-cellapp ordinal for color coding
    float min_x{0.f};
    float min_z{0.f};
    float max_x{0.f};
    float max_z{0.f};
    float load{0.f};
    uint32_t entity_count{0};
  };
  std::vector<LeafRect> leaves;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kSpaceBspGeometry),
                                   "baseapp::SpaceBspGeometry",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.WritePackedInt(static_cast<uint32_t>(leaves.size()));
    for (const auto& l : leaves) {
      w.Write(l.cell_id);
      w.Write<uint8_t>(l.owner_index);
      w.Write(l.min_x);
      w.Write(l.min_z);
      w.Write(l.max_x);
      w.Write(l.max_z);
      w.Write(l.load);
      w.Write(l.entity_count);
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<SpaceBspGeometry> {
    auto sid = r.Read<uint32_t>();
    auto n = r.ReadPackedInt();
    if (!sid || !n) return Error{ErrorCode::kInvalidArgument, "SpaceBspGeometry: truncated"};
    SpaceBspGeometry msg;
    msg.space_id = *sid;
    msg.leaves.reserve(*n);
    for (uint32_t i = 0; i < *n; ++i) {
      auto cid = r.Read<uint32_t>();
      auto oi = r.Read<uint8_t>();
      auto x0 = r.Read<float>();
      auto z0 = r.Read<float>();
      auto x1 = r.Read<float>();
      auto z1 = r.Read<float>();
      auto load = r.Read<float>();
      auto entities = r.Read<uint32_t>();
      if (!cid || !oi || !x0 || !z0 || !x1 || !z1 || !load || !entities)
        return Error{ErrorCode::kInvalidArgument, "SpaceBspGeometry: leaf entry truncated"};
      msg.leaves.push_back({*cid, *oi, *x0, *z0, *x1, *z1, *load, *entities});
    }
    return msg;
  }
};
static_assert(NetworkMessage<SpaceBspGeometry>);

}  // namespace atlas::baseapp

#endif  // ATLAS_SERVER_BASEAPP_BASEAPP_MESSAGES_H_
