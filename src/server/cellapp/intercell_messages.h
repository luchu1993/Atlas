#ifndef ATLAS_SERVER_CELLAPP_INTERCELL_MESSAGES_H_
#define ATLAS_SERVER_CELLAPP_INTERCELL_MESSAGES_H_

#include <cstdint>
#include <vector>

#include "math/vector3.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"
#include "server/entity_types.h"

namespace atlas::cellapp {

// Seeds a Ghost replica with the Real entity's other-audience snapshot
// baseline. `event_seq` / `volatile_seq` give the receiving side a
// starting point for ordering subsequent GhostDelta /
// GhostPositionUpdate messages (they slot directly into
// ReplicationState's latest_*_seq). `real_cellapp_addr` is the channel
// the Ghost uses to forward anything back to the Real side.

struct CreateGhost {
  EntityID entity_id{kInvalidEntityID};
  uint16_t type_id{0};
  SpaceID space_id{kInvalidSpaceID};
  math::Vector3 position{0.f, 0.f, 0.f};
  math::Vector3 direction{1.f, 0.f, 0.f};
  bool on_ground{false};
  Address real_cellapp_addr;
  Address base_addr;
  uint64_t event_seq{0};
  uint64_t volatile_seq{0};
  std::vector<std::byte> other_snapshot;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kCreateGhost),
                                   "cellapp::CreateGhost",
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
    w.Write(real_cellapp_addr.Ip());
    w.Write(real_cellapp_addr.Port());
    w.Write(base_addr.Ip());
    w.Write(base_addr.Port());
    w.Write(event_seq);
    w.Write(volatile_seq);
    w.WritePackedInt(static_cast<uint32_t>(other_snapshot.size()));
    if (!other_snapshot.empty()) w.WriteBytes(std::span<const std::byte>(other_snapshot));
  }

  static auto Deserialize(BinaryReader& r) -> Result<CreateGhost> {
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
    auto rip = r.Read<uint32_t>();
    auto rport = r.Read<uint16_t>();
    auto bip = r.Read<uint32_t>();
    auto bport = r.Read<uint16_t>();
    auto es = r.Read<uint64_t>();
    auto vs = r.Read<uint64_t>();
    auto snlen = r.ReadPackedInt();
    if (!eid || !ti || !sid || !px || !py || !pz || !dx || !dy || !dz || !og || !rip || !rport ||
        !bip || !bport || !es || !vs || !snlen)
      return Error{ErrorCode::kInvalidArgument, "CreateGhost: truncated"};
    CreateGhost msg;
    msg.entity_id = *eid;
    msg.type_id = *ti;
    msg.space_id = *sid;
    msg.position = {*px, *py, *pz};
    msg.direction = {*dx, *dy, *dz};
    msg.on_ground = (*og != 0);
    msg.real_cellapp_addr = Address(*rip, *rport);
    msg.base_addr = Address(*bip, *bport);
    msg.event_seq = *es;
    msg.volatile_seq = *vs;
    if (*snlen > 0) {
      auto data = r.ReadBytes(*snlen);
      if (!data) return Error{ErrorCode::kInvalidArgument, "CreateGhost: snapshot truncated"};
      msg.other_snapshot.assign(data->begin(), data->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<CreateGhost>);

struct DeleteGhost {
  EntityID entity_id{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kDeleteGhost),
                                   "cellapp::DeleteGhost",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(EntityID)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(entity_id); }

  static auto Deserialize(BinaryReader& r) -> Result<DeleteGhost> {
    auto eid = r.Read<uint32_t>();
    if (!eid) return Error{ErrorCode::kInvalidArgument, "DeleteGhost: truncated"};
    DeleteGhost msg;
    msg.entity_id = *eid;
    return msg;
  }
};
static_assert(NetworkMessage<DeleteGhost>);

// Volatile (latest-wins) position + orientation. `volatile_seq` is the
// Real side's ReplicationState::latest_volatile_seq at emit time; the
// Ghost discards any frame whose seq is <= what it already holds.

struct GhostPositionUpdate {
  EntityID entity_id{kInvalidEntityID};
  math::Vector3 position{0.f, 0.f, 0.f};
  math::Vector3 direction{1.f, 0.f, 0.f};
  bool on_ground{false};
  uint64_t volatile_seq{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellApp::kGhostPositionUpdate),
        "cellapp::GhostPositionUpdate",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + 6 * sizeof(float) + sizeof(uint8_t) + sizeof(uint64_t)),
        MessageReliability::kReliable,
        MessageUrgency::kBatched};
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
    w.Write(volatile_seq);
  }

  static auto Deserialize(BinaryReader& r) -> Result<GhostPositionUpdate> {
    auto eid = r.Read<uint32_t>();
    auto px = r.Read<float>();
    auto py = r.Read<float>();
    auto pz = r.Read<float>();
    auto dx = r.Read<float>();
    auto dy = r.Read<float>();
    auto dz = r.Read<float>();
    auto og = r.Read<uint8_t>();
    auto vs = r.Read<uint64_t>();
    if (!eid || !px || !py || !pz || !dx || !dy || !dz || !og || !vs)
      return Error{ErrorCode::kInvalidArgument, "GhostPositionUpdate: truncated"};
    GhostPositionUpdate msg;
    msg.entity_id = *eid;
    msg.position = {*px, *py, *pz};
    msg.direction = {*dx, *dy, *dz};
    msg.on_ground = (*og != 0);
    msg.volatile_seq = *vs;
    return msg;
  }
};
static_assert(NetworkMessage<GhostPositionUpdate>);

// Other-audience delta (output of DeltaSyncEmitter::SerializeOtherDelta
// with the _dirtyFlags & OtherVisibleMask filter). `event_seq` is the
// Real side's ReplicationState::latest_event_seq at emit time - Ghost
// appends into its own history deque keyed by that seq. Gaps larger
// than kReplicationHistoryWindow (8 frames) must be healed by a
// GhostSnapshotRefresh rather than by silent state loss.

struct GhostDelta {
  EntityID entity_id{kInvalidEntityID};
  uint64_t event_seq{0};
  std::vector<std::byte> other_delta;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kGhostDelta),
                                   "cellapp::GhostDelta",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(event_seq);
    w.WritePackedInt(static_cast<uint32_t>(other_delta.size()));
    if (!other_delta.empty()) w.WriteBytes(std::span<const std::byte>(other_delta));
  }

  static auto Deserialize(BinaryReader& r) -> Result<GhostDelta> {
    auto eid = r.Read<uint32_t>();
    auto es = r.Read<uint64_t>();
    auto dlen = r.ReadPackedInt();
    if (!eid || !es || !dlen) return Error{ErrorCode::kInvalidArgument, "GhostDelta: truncated"};
    GhostDelta msg;
    msg.entity_id = *eid;
    msg.event_seq = *es;
    if (*dlen > 0) {
      auto data = r.ReadBytes(*dlen);
      if (!data) return Error{ErrorCode::kInvalidArgument, "GhostDelta: delta truncated"};
      msg.other_delta.assign(data->begin(), data->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<GhostDelta>);

// Sent when the Real side detects that a given Ghost's event_seq trails
// by more than kReplicationHistoryWindow (= 8) frames -
// continuous-delta catch-up is no longer feasible, so rebase the
// Ghost's other_snapshot to a fresh full snapshot at `event_seq` and
// let future GhostDelta streams resume from there.

struct GhostSnapshotRefresh {
  EntityID entity_id{kInvalidEntityID};
  uint64_t event_seq{0};
  std::vector<std::byte> other_snapshot;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kGhostSnapshotRefresh),
                                   "cellapp::GhostSnapshotRefresh",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(event_seq);
    w.WritePackedInt(static_cast<uint32_t>(other_snapshot.size()));
    if (!other_snapshot.empty()) w.WriteBytes(std::span<const std::byte>(other_snapshot));
  }

  static auto Deserialize(BinaryReader& r) -> Result<GhostSnapshotRefresh> {
    auto eid = r.Read<uint32_t>();
    auto es = r.Read<uint64_t>();
    auto snlen = r.ReadPackedInt();
    if (!eid || !es || !snlen)
      return Error{ErrorCode::kInvalidArgument, "GhostSnapshotRefresh: truncated"};
    GhostSnapshotRefresh msg;
    msg.entity_id = *eid;
    msg.event_seq = *es;
    if (*snlen > 0) {
      auto data = r.ReadBytes(*snlen);
      if (!data)
        return Error{ErrorCode::kInvalidArgument, "GhostSnapshotRefresh: snapshot truncated"};
      msg.other_snapshot.assign(data->begin(), data->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<GhostSnapshotRefresh>);

// Sent after a successful Offload - the ghost redirects subsequent
// forwarded-RPC / real-reply traffic to the new Real address.

struct GhostSetReal {
  EntityID entity_id{kInvalidEntityID};
  Address new_real_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellApp::kGhostSetReal),
        "cellapp::GhostSetReal",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(new_real_addr.Ip());
    w.Write(new_real_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<GhostSetReal> {
    auto eid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    if (!eid || !ip || !port) return Error{ErrorCode::kInvalidArgument, "GhostSetReal: truncated"};
    GhostSetReal msg;
    msg.entity_id = *eid;
    msg.new_real_addr = Address(*ip, *port);
    return msg;
  }
};
static_assert(NetworkMessage<GhostSetReal>);

// Pre-Offload notification; lets ghosts buffer traffic for the
// transition window rather than drop it.

struct GhostSetNextReal {
  EntityID entity_id{kInvalidEntityID};
  Address next_real_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellApp::kGhostSetNextReal),
        "cellapp::GhostSetNextReal",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(next_real_addr.Ip());
    w.Write(next_real_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<GhostSetNextReal> {
    auto eid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    if (!eid || !ip || !port)
      return Error{ErrorCode::kInvalidArgument, "GhostSetNextReal: truncated"};
    GhostSetNextReal msg;
    msg.entity_id = *eid;
    msg.next_real_addr = Address(*ip, *port);
    return msg;
  }
};
static_assert(NetworkMessage<GhostSetNextReal>);

// Carries the full state needed to promote a Ghost into Real on the
// receiving side, or to materialise a fresh Real if the ghost does not
// yet exist there. `persistent_blob` is the C# ServerEntity.Serialize
// output.
// owner_snapshot / other_snapshot / latest_*_seq let the receiver fill
// ReplicationState on arrival without needing to wait for the first C#
// publish_replication_frame tick. `existing_haunts` is the current
// Real's Haunt list so the new Real can immediately resume broadcasts.

struct OffloadEntity {
  EntityID entity_id{kInvalidEntityID};
  uint16_t type_id{0};
  SpaceID space_id{kInvalidSpaceID};
  math::Vector3 position{0.f, 0.f, 0.f};
  math::Vector3 direction{1.f, 0.f, 0.f};
  bool on_ground{false};
  Address base_addr;
  std::vector<std::byte> persistent_blob;
  std::vector<std::byte> owner_snapshot;
  std::vector<std::byte> other_snapshot;
  uint64_t latest_event_seq{0};
  uint64_t latest_volatile_seq{0};
  // Serialized Controller state (MoveToPoint, Timer, Proximity)
  // captured by `SerializeControllersForMigration` at send-time
  // BEFORE ConvertRealToGhost's StopAll runs, so a mid-motion
  // controller resumes on the receiver at the same waypoint /
  // remaining interval / proximity membership without the script
  // needing to re-arm. Empty when the sender had no live controllers.
  std::vector<std::byte> controller_data;
  std::vector<Address> existing_haunts;

  // Witness state preservation across the Offload boundary. The sender's
  // Witness is torn down by ConvertRealToGhost; without these fields
  // the receiver would re-enable with CellAppConfig defaults and
  // silently drop any script-level SetAoIRadius.
  // `has_witness==false` => the other two floats are ignored.
  bool has_witness{false};
  float aoi_radius{0.f};
  float aoi_hysteresis{0.f};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kOffloadEntity),
                                   "cellapp::OffloadEntity",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
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
    w.WritePackedInt(static_cast<uint32_t>(persistent_blob.size()));
    if (!persistent_blob.empty()) w.WriteBytes(std::span<const std::byte>(persistent_blob));
    w.WritePackedInt(static_cast<uint32_t>(owner_snapshot.size()));
    if (!owner_snapshot.empty()) w.WriteBytes(std::span<const std::byte>(owner_snapshot));
    w.WritePackedInt(static_cast<uint32_t>(other_snapshot.size()));
    if (!other_snapshot.empty()) w.WriteBytes(std::span<const std::byte>(other_snapshot));
    w.Write(latest_event_seq);
    w.Write(latest_volatile_seq);
    w.WritePackedInt(static_cast<uint32_t>(controller_data.size()));
    if (!controller_data.empty()) w.WriteBytes(std::span<const std::byte>(controller_data));
    w.WritePackedInt(static_cast<uint32_t>(existing_haunts.size()));
    for (const auto& a : existing_haunts) {
      w.Write(a.Ip());
      w.Write(a.Port());
    }
    // Witness state - appended at the tail so Deserialize can treat the
    // block as optional via `BinaryReader::Remaining() >= 9`. Keeps
    // wire-format back-compat with older-boundary tests.
    w.Write(static_cast<uint8_t>(has_witness ? 1 : 0));
    w.Write(aoi_radius);
    w.Write(aoi_hysteresis);
  }

  static auto Deserialize(BinaryReader& r) -> Result<OffloadEntity> {
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
    auto bip = r.Read<uint32_t>();
    auto bport = r.Read<uint16_t>();
    auto pblen = r.ReadPackedInt();
    if (!eid || !ti || !sid || !px || !py || !pz || !dx || !dy || !dz || !og || !bip || !bport ||
        !pblen)
      return Error{ErrorCode::kInvalidArgument, "OffloadEntity: truncated"};
    OffloadEntity msg;
    msg.entity_id = *eid;
    msg.type_id = *ti;
    msg.space_id = *sid;
    msg.position = {*px, *py, *pz};
    msg.direction = {*dx, *dy, *dz};
    msg.on_ground = (*og != 0);
    msg.base_addr = Address(*bip, *bport);
    if (*pblen > 0) {
      auto data = r.ReadBytes(*pblen);
      if (!data) return Error{ErrorCode::kInvalidArgument, "OffloadEntity: blob truncated"};
      msg.persistent_blob.assign(data->begin(), data->end());
    }
    auto oslen = r.ReadPackedInt();
    if (!oslen)
      return Error{ErrorCode::kInvalidArgument, "OffloadEntity: owner_snapshot len truncated"};
    if (*oslen > 0) {
      auto data = r.ReadBytes(*oslen);
      if (!data)
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: owner_snapshot truncated"};
      msg.owner_snapshot.assign(data->begin(), data->end());
    }
    auto tslen = r.ReadPackedInt();
    if (!tslen)
      return Error{ErrorCode::kInvalidArgument, "OffloadEntity: other_snapshot len truncated"};
    if (*tslen > 0) {
      auto data = r.ReadBytes(*tslen);
      if (!data)
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: other_snapshot truncated"};
      msg.other_snapshot.assign(data->begin(), data->end());
    }
    auto les = r.Read<uint64_t>();
    auto lvs = r.Read<uint64_t>();
    auto cdlen = r.ReadPackedInt();
    if (!les || !lvs || !cdlen)
      return Error{ErrorCode::kInvalidArgument, "OffloadEntity: seq/controller_len truncated"};
    msg.latest_event_seq = *les;
    msg.latest_volatile_seq = *lvs;
    if (*cdlen > 0) {
      auto data = r.ReadBytes(*cdlen);
      if (!data)
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: controller_data truncated"};
      msg.controller_data.assign(data->begin(), data->end());
    }
    auto hcnt = r.ReadPackedInt();
    if (!hcnt) return Error{ErrorCode::kInvalidArgument, "OffloadEntity: haunt_count truncated"};
    msg.existing_haunts.reserve(*hcnt);
    for (uint32_t i = 0; i < *hcnt; ++i) {
      auto hip = r.Read<uint32_t>();
      auto hport = r.Read<uint16_t>();
      if (!hip || !hport)
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: haunt addr truncated"};
      msg.existing_haunts.emplace_back(*hip, *hport);
    }
    // Optional witness-state tail. Absent => has_witness=false (the
    // default), matching older-format payloads without regressing the
    // Witness behaviour: the receiver leaves no Witness attached, which
    // is the legacy observable state.
    if (r.Remaining() >= sizeof(uint8_t) + 2 * sizeof(float)) {
      auto hw = r.Read<uint8_t>();
      auto rad = r.Read<float>();
      auto hyst = r.Read<float>();
      if (!hw || !rad || !hyst)
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: witness tail truncated"};
      msg.has_witness = (*hw != 0);
      msg.aoi_radius = *rad;
      msg.aoi_hysteresis = *hyst;
    }
    return msg;
  }
};
static_assert(NetworkMessage<OffloadEntity>);

// `success == false` means the destination rejected the offload (no
// such Space, unknown type, allocation failure, ...); the source CellApp
// keeps the entity put.

struct OffloadEntityAck {
  EntityID entity_id{kInvalidEntityID};
  bool success{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kOffloadEntityAck),
                                   "cellapp::OffloadEntityAck",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
  }

  static auto Deserialize(BinaryReader& r) -> Result<OffloadEntityAck> {
    auto eid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    if (!eid || !ok) return Error{ErrorCode::kInvalidArgument, "OffloadEntityAck: truncated"};
    OffloadEntityAck msg;
    msg.entity_id = *eid;
    msg.success = (*ok != 0);
    return msg;
  }
};
static_assert(NetworkMessage<OffloadEntityAck>);

}  // namespace atlas::cellapp

#endif  // ATLAS_SERVER_CELLAPP_INTERCELL_MESSAGES_H_
