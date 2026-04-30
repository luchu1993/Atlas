#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_MESSAGES_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_MESSAGES_H_

#include <cstdint>
#include <vector>

#include "math/vector3.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"
#include "server/entity_types.h"

// ============================================================================
// CellApp messages (IDs 3000–3999)
//
// Inbound-to-CellApp. CellApp's outbound traffic reuses BaseApp's message
// enum (baseapp::CellEntityCreated, baseapp::BroadcastRpcFromCell, …) because
// the physical receiver is BaseApp and message IDs share one flat space.
//
// RPC split — exposed (client-facing) vs internal (server-only):
//   - ClientCellRpcForward (3003): client-initiated, REAL_ONLY, carries
//     `source_entity_id` stamped by BaseApp. CellApp re-validates
//     direction + exposed + OwnClient binding (four-layer defence).
//   - InternalCellRpc (3004): server-internal Base → Cell, REAL_ONLY, no
//     exposed check because BaseApp is trusted.
// Never collapse the two — a single "is_from_client" bool would be
// another bit an attacker could aim for.
// ============================================================================

namespace atlas::cellapp {

// ----------------------------------------------------------------------------
// CreateCellEntity  (BaseApp → CellApp, ID 3000)
//
// BaseApp asks the cell to materialise a cell-side counterpart for a Base
// entity. `script_init_data` is the opaque Cell-side initialisation blob
// produced by the script layer (anything from defaults to persisted
// fields). CellApp composes it with runtime-supplied fields (space, base
// mailbox, position/direction, on_ground) into the full restore blob
// consumed by the C# RestoreEntity callback.
// ----------------------------------------------------------------------------

struct CreateCellEntity {
  EntityID entity_id{kInvalidEntityID};
  uint16_t type_id{0};
  SpaceID space_id{kInvalidSpaceID};
  math::Vector3 position{0.f, 0.f, 0.f};
  math::Vector3 direction{1.f, 0.f, 0.f};
  bool on_ground{false};
  Address base_addr;  // where to send CellEntityCreated back
  uint32_t request_id{0};
  std::vector<std::byte> script_init_data;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kCreateCellEntity),
                                   "cellapp::CreateCellEntity",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(type_id);
    w.Write(space_id);
    w.Write(position.x);
    w.Write(position.y);
    w.Write(position.z);
    w.Write(direction.x);
    w.Write(direction.y);
    w.Write(direction.z);
    w.Write(static_cast<uint8_t>(on_ground ? 1 : 0));
    w.Write(base_addr.Ip());
    w.Write(base_addr.Port());
    w.Write(request_id);
    w.WritePackedInt(static_cast<uint32_t>(script_init_data.size()));
    if (!script_init_data.empty()) w.WriteBytes(std::span<const std::byte>(script_init_data));
  }

  static auto Deserialize(BinaryReader& r) -> Result<CreateCellEntity> {
    auto eid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    auto sid = r.Read<uint32_t>();
    auto px = r.Read<float>();
    auto py = r.Read<float>();
    auto pz = r.Read<float>();
    auto dx = r.Read<float>();
    auto dy = r.Read<float>();
    auto dz = r.Read<float>();
    auto og = r.Read<uint8_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto rid = r.Read<uint32_t>();
    auto slen = r.ReadPackedInt();
    if (!eid || !ti || !sid || !px || !py || !pz || !dx || !dy || !dz || !og || !ip || !port ||
        !rid || !slen)
      return Error{ErrorCode::kInvalidArgument, "CreateCellEntity: truncated"};
    CreateCellEntity msg;
    msg.entity_id = *eid;
    msg.type_id = *ti;
    msg.space_id = *sid;
    msg.position = {*px, *py, *pz};
    msg.direction = {*dx, *dy, *dz};
    msg.on_ground = (*og != 0);
    msg.base_addr = Address(*ip, *port);
    msg.request_id = *rid;
    if (*slen > 0) {
      auto data = r.ReadBytes(*slen);
      if (!data)
        return Error{ErrorCode::kInvalidArgument, "CreateCellEntity: script data truncated"};
      msg.script_init_data.assign(data->begin(), data->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<CreateCellEntity>);

// ----------------------------------------------------------------------------
// DestroyCellEntity  (BaseApp → CellApp, ID 3002)
//
// Targeted by the unified entity_id (cluster-stable, allocated by
// DBApp's IDClient).
// ----------------------------------------------------------------------------

struct DestroyCellEntity {
  EntityID entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kDestroyCellEntity),
                                   "cellapp::DestroyCellEntity",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(EntityID)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(entity_id); }

  static auto Deserialize(BinaryReader& r) -> Result<DestroyCellEntity> {
    auto eid = r.Read<uint32_t>();
    if (!eid) return Error{ErrorCode::kInvalidArgument, "DestroyCellEntity: truncated"};
    DestroyCellEntity msg;
    msg.entity_id = *eid;
    return msg;
  }
};
static_assert(NetworkMessage<DestroyCellEntity>);

// ----------------------------------------------------------------------------
// ClientCellRpcForward  (BaseApp → CellApp, ID 3003)
//
// Exposed (client-facing) RPC forwarded from BaseApp. `source_entity_id`
// is stamped by BaseApp from the client's proxy binding and cannot be
// forged by the client. OWN_CLIENT methods require source == target.
// ----------------------------------------------------------------------------

struct ClientCellRpcForward {
  EntityID target_entity_id{kInvalidEntityID};
  EntityID source_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kClientCellRpcForward),
                                   "cellapp::ClientCellRpcForward",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WritePackedInt(target_entity_id);
    w.WritePackedInt(source_entity_id);
    w.Write(rpc_id);
    w.WritePackedInt(static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) w.WriteBytes(std::span<const std::byte>(payload));
  }

  static auto Deserialize(BinaryReader& r) -> Result<ClientCellRpcForward> {
    auto tid = r.ReadPackedInt();
    auto sid = r.ReadPackedInt();
    auto rid = r.Read<uint32_t>();
    auto plen = r.ReadPackedInt();
    if (!tid || !sid || !rid || !plen)
      return Error{ErrorCode::kInvalidArgument, "ClientCellRpcForward: truncated"};
    ClientCellRpcForward msg;
    msg.target_entity_id = *tid;
    msg.source_entity_id = *sid;
    msg.rpc_id = *rid;
    if (*plen > 0) {
      auto pdata = r.ReadBytes(*plen);
      if (!pdata)
        return Error{ErrorCode::kInvalidArgument, "ClientCellRpcForward: payload truncated"};
      msg.payload.assign(pdata->begin(), pdata->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<ClientCellRpcForward>);

// ----------------------------------------------------------------------------
// InternalCellRpc  (BaseApp → CellApp, ID 3004)
//
// Server-to-server trusted RPC; skips exposed/source validation.
// ----------------------------------------------------------------------------

struct InternalCellRpc {
  EntityID target_entity_id{kInvalidEntityID};
  uint32_t rpc_id{0};
  std::vector<std::byte> payload;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kInternalCellRpc),
                                   "cellapp::InternalCellRpc",
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

  static auto Deserialize(BinaryReader& r) -> Result<InternalCellRpc> {
    auto tid = r.ReadPackedInt();
    auto rid = r.Read<uint32_t>();
    auto plen = r.ReadPackedInt();
    if (!tid || !rid || !plen)
      return Error{ErrorCode::kInvalidArgument, "InternalCellRpc: truncated"};
    InternalCellRpc msg;
    msg.target_entity_id = *tid;
    msg.rpc_id = *rid;
    if (*plen > 0) {
      auto pdata = r.ReadBytes(*plen);
      if (!pdata) return Error{ErrorCode::kInvalidArgument, "InternalCellRpc: payload truncated"};
      msg.payload.assign(pdata->begin(), pdata->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<InternalCellRpc>);

// ----------------------------------------------------------------------------
// CreateSpace  (management/script → CellApp, ID 3010)
// ----------------------------------------------------------------------------

struct CreateSpace {
  SpaceID space_id{kInvalidSpaceID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kCreateSpace),
                                   "cellapp::CreateSpace",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(SpaceID)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(space_id); }

  static auto Deserialize(BinaryReader& r) -> Result<CreateSpace> {
    auto sid = r.Read<uint32_t>();
    if (!sid) return Error{ErrorCode::kInvalidArgument, "CreateSpace: truncated"};
    CreateSpace msg;
    msg.space_id = *sid;
    return msg;
  }
};
static_assert(NetworkMessage<CreateSpace>);

// ----------------------------------------------------------------------------
// DestroySpace  (management/script → CellApp, ID 3011)
// ----------------------------------------------------------------------------

struct DestroySpace {
  SpaceID space_id{kInvalidSpaceID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kDestroySpace),
                                   "cellapp::DestroySpace",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(SpaceID)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(space_id); }

  static auto Deserialize(BinaryReader& r) -> Result<DestroySpace> {
    auto sid = r.Read<uint32_t>();
    if (!sid) return Error{ErrorCode::kInvalidArgument, "DestroySpace: truncated"};
    DestroySpace msg;
    msg.space_id = *sid;
    return msg;
  }
};
static_assert(NetworkMessage<DestroySpace>);

// ----------------------------------------------------------------------------
// AvatarUpdate  (BaseApp → CellApp, ID 3020)
//
// Client-authoritative position update. CellApp applies:
//   1) std::isfinite on all three components (reject NaN/Inf);
//   2) single-tick displacement bound (reject teleports).
// ----------------------------------------------------------------------------

struct AvatarUpdate {
  EntityID entity_id{kInvalidEntityID};
  math::Vector3 position{0.f, 0.f, 0.f};
  math::Vector3 direction{1.f, 0.f, 0.f};
  bool on_ground{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellApp::kAvatarUpdate), "cellapp::AvatarUpdate",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + 6 * sizeof(float) + sizeof(uint8_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(position.x);
    w.Write(position.y);
    w.Write(position.z);
    w.Write(direction.x);
    w.Write(direction.y);
    w.Write(direction.z);
    w.Write(static_cast<uint8_t>(on_ground ? 1 : 0));
  }

  static auto Deserialize(BinaryReader& r) -> Result<AvatarUpdate> {
    auto eid = r.Read<uint32_t>();
    auto px = r.Read<float>();
    auto py = r.Read<float>();
    auto pz = r.Read<float>();
    auto dx = r.Read<float>();
    auto dy = r.Read<float>();
    auto dz = r.Read<float>();
    auto og = r.Read<uint8_t>();
    if (!eid || !px || !py || !pz || !dx || !dy || !dz || !og)
      return Error{ErrorCode::kInvalidArgument, "AvatarUpdate: truncated"};
    AvatarUpdate msg;
    msg.entity_id = *eid;
    msg.position = {*px, *py, *pz};
    msg.direction = {*dx, *dy, *dz};
    msg.on_ground = (*og != 0);
    return msg;
  }
};
static_assert(NetworkMessage<AvatarUpdate>);

// ----------------------------------------------------------------------------
// EnableWitness  (BaseApp → CellApp, ID 3021)
//
// Attaches an AoI witness to a cell entity. Fired from BaseApp's
// client-bind hooks (Proxy::BindClient → this message). The witness
// uses config-driven defaults (cellApp/default_aoi_radius,
// cellApp/default_aoi_hysteresis); script-level overrides arrive via
// a subsequent SetAoIRadius message.
// ----------------------------------------------------------------------------

struct EnableWitness {
  EntityID entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kEnableWitness),
                                   "cellapp::EnableWitness",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(EntityID)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(entity_id); }

  static auto Deserialize(BinaryReader& r) -> Result<EnableWitness> {
    auto eid = r.Read<uint32_t>();
    if (!eid) return Error{ErrorCode::kInvalidArgument, "EnableWitness: truncated"};
    EnableWitness msg;
    msg.entity_id = *eid;
    return msg;
  }
};
static_assert(NetworkMessage<EnableWitness>);

// ----------------------------------------------------------------------------
// DisableWitness  (BaseApp/script → CellApp, ID 3022)
// ----------------------------------------------------------------------------

struct DisableWitness {
  EntityID entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kDisableWitness),
                                   "cellapp::DisableWitness",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(EntityID)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(entity_id); }

  static auto Deserialize(BinaryReader& r) -> Result<DisableWitness> {
    auto eid = r.Read<uint32_t>();
    if (!eid) return Error{ErrorCode::kInvalidArgument, "DisableWitness: truncated"};
    DisableWitness msg;
    msg.entity_id = *eid;
    return msg;
  }
};
static_assert(NetworkMessage<DisableWitness>);

// ----------------------------------------------------------------------------
// SetAoIRadius  (BaseApp/script → CellApp, ID 3023)
//
// Runtime AoI radius + hysteresis adjustment for an already-witnessed
// cell entity. Mutates the Witness and reshapes its trigger in place.
// The handler silently drops the message if the target has no
// Witness attached (log-warn; the typical cause is a race where bind
// notifications arrive after Witness teardown).
// ----------------------------------------------------------------------------

struct SetAoIRadius {
  EntityID entity_id{kInvalidEntityID};
  float radius{0.f};
  float hysteresis{0.f};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kSetAoIRadius),
                                   "cellapp::SetAoIRadius",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(EntityID) + 2 * sizeof(float)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(radius);
    w.Write(hysteresis);
  }

  static auto Deserialize(BinaryReader& r) -> Result<SetAoIRadius> {
    auto eid = r.Read<uint32_t>();
    auto rad = r.Read<float>();
    auto hyst = r.Read<float>();
    if (!eid || !rad || !hyst) return Error{ErrorCode::kInvalidArgument, "SetAoIRadius: truncated"};
    SetAoIRadius msg;
    msg.entity_id = *eid;
    msg.radius = *rad;
    msg.hysteresis = *hyst;
    return msg;
  }
};
static_assert(NetworkMessage<SetAoIRadius>);

}  // namespace atlas::cellapp

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_MESSAGES_H_
