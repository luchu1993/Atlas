#ifndef ATLAS_SERVER_CELLAPP_INTERCELL_MESSAGES_H_
#define ATLAS_SERVER_CELLAPP_INTERCELL_MESSAGES_H_

#include <cstdint>
#include <vector>

#include "cellappmgr/cellappmgr_messages.h"
#include "math/vector3.h"
#include "movement_position_history_store.h"
#include "movement_sim/movement_codec.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"
#include "server/entity_types.h"

namespace atlas::cellapp {

// Per-peer Witness state for Avatar Offload; only entity-owned seqs move.
// Witness-local tick scheduling is rebuilt from the destination tick_count_.
struct WitnessAoIEntry {
  EntityID id{kInvalidEntityID};
  uint64_t last_event_seq{0};
  uint64_t last_volatile_seq{0};
};

// Seeds a Ghost with Real's other-audience baseline and optional Cell backup.
// Seqs let later GhostDelta / GhostPositionUpdate continue ordering.
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
  std::vector<std::byte> persistent_blob;

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
    w.WritePackedInt(static_cast<uint32_t>(persistent_blob.size()));
    if (!persistent_blob.empty()) w.WriteBytes(std::span<const std::byte>(persistent_blob));
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
    if (r.Remaining() == 0) return msg;
    auto pblen = r.ReadPackedInt();
    if (!pblen)
      return Error{ErrorCode::kInvalidArgument, "CreateGhost: persistent blob len truncated"};
    if (*pblen > 0) {
      auto data = r.ReadBytes(*pblen);
      if (!data)
        return Error{ErrorCode::kInvalidArgument, "CreateGhost: persistent blob truncated"};
      msg.persistent_blob.assign(data->begin(), data->end());
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

// Other-audience delta from DeltaSyncEmitter; event_seq is Real's latest.
// Gaps beyond the history window are healed by GhostSnapshotRefresh.

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

// Re-bases a Ghost's other_snapshot after a history gap, and can refresh
// its Cell backup even when the public snapshot is unchanged.
struct GhostSnapshotRefresh {
  EntityID entity_id{kInvalidEntityID};
  uint64_t event_seq{0};
  std::vector<std::byte> other_snapshot;
  std::vector<std::byte> persistent_blob;

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
    w.WritePackedInt(static_cast<uint32_t>(persistent_blob.size()));
    if (!persistent_blob.empty()) w.WriteBytes(std::span<const std::byte>(persistent_blob));
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
    if (r.Remaining() == 0) return msg;
    auto pblen = r.ReadPackedInt();
    if (!pblen)
      return Error{ErrorCode::kInvalidArgument,
                   "GhostSnapshotRefresh: persistent blob len truncated"};
    if (*pblen > 0) {
      auto data = r.ReadBytes(*pblen);
      if (!data)
        return Error{ErrorCode::kInvalidArgument,
                     "GhostSnapshotRefresh: persistent blob truncated"};
      msg.persistent_blob.assign(data->begin(), data->end());
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
  std::vector<std::byte> controller_data;
  std::vector<Address> existing_haunts;

  bool has_witness{false};
  float aoi_radius{0.f};
  float aoi_hysteresis{0.f};
  uint32_t cell_epoch{0};
  bool is_local{false};
  cellappmgr::CellID target_cell_id{0};
  uint64_t geometry_version{0};
  std::vector<WitnessAoIEntry> aoi_entries;
  bool has_movement_state{false};
  movement::MovementState movement_state;
  std::vector<MovementPositionSample> movement_position_history;
  bool has_movement_command{false};
  movement::MovementCommand movement_command;
  // Script-initiated cross-space teleport. Sender ships geometry_version=0 so
  // the receiver places via its own BSP instead of validating source geometry.
  bool is_teleport{false};

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
    w.Write(static_cast<uint8_t>(has_witness ? 1 : 0));
    w.Write(aoi_radius);
    w.Write(aoi_hysteresis);
    w.Write(cell_epoch);
    w.Write(static_cast<uint8_t>(is_local ? 1 : 0));
    w.WritePackedInt(static_cast<uint32_t>(aoi_entries.size()));
    for (const auto& e : aoi_entries) {
      w.Write(e.id);
      w.Write(e.last_event_seq);
      w.Write(e.last_volatile_seq);
    }
    w.Write(target_cell_id);
    w.Write(geometry_version);
    w.Write(static_cast<uint8_t>(has_movement_state ? 1 : 0));
    if (has_movement_state) movement::SerializeMovementState(w, movement_state);
    w.WritePackedInt(static_cast<uint32_t>(movement_position_history.size()));
    for (const auto& sample : movement_position_history) {
      w.Write(sample.server_tick);
      movement::SerializeMovementState(w, sample.state);
    }
    w.Write(static_cast<uint8_t>(has_movement_command ? 1 : 0));
    if (has_movement_command) movement::SerializeMovementCommand(w, movement_command);
    w.Write(static_cast<uint8_t>(is_teleport ? 1 : 0));
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
    // Absent witness tail keeps the legacy no-Witness destination state.
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
    if (r.Remaining() >= sizeof(uint32_t)) {
      auto ce = r.Read<uint32_t>();
      if (!ce) return Error{ErrorCode::kInvalidArgument, "OffloadEntity: cell_epoch truncated"};
      msg.cell_epoch = *ce;
    }
    if (r.Remaining() >= sizeof(uint8_t)) {
      auto il = r.Read<uint8_t>();
      if (!il) return Error{ErrorCode::kInvalidArgument, "OffloadEntity: is_local truncated"};
      msg.is_local = (*il != 0);
    }
    // Older payloads omit AoI entries and re-enter everything.
    if (r.Remaining() >= 1) {
      auto cnt = r.ReadPackedInt();
      if (!cnt) return Error{ErrorCode::kInvalidArgument, "OffloadEntity: aoi count truncated"};
      msg.aoi_entries.reserve(*cnt);
      for (uint32_t i = 0; i < *cnt; ++i) {
        auto id = r.Read<uint32_t>();
        auto evt = r.Read<uint64_t>();
        auto vol = r.Read<uint64_t>();
        if (!id || !evt || !vol)
          return Error{ErrorCode::kInvalidArgument, "OffloadEntity: aoi entry truncated"};
        msg.aoi_entries.push_back(WitnessAoIEntry{*id, *evt, *vol});
      }
    }
    if (r.Remaining() >= sizeof(uint32_t) + sizeof(uint64_t)) {
      auto tc = r.Read<uint32_t>();
      auto gv = r.Read<uint64_t>();
      if (!tc || !gv)
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: geometry tail truncated"};
      msg.target_cell_id = *tc;
      msg.geometry_version = *gv;
    }
    if (r.Remaining() >= sizeof(uint8_t)) {
      auto hms = r.Read<uint8_t>();
      if (!hms)
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: movement tail truncated"};
      msg.has_movement_state = (*hms != 0);
      if (msg.has_movement_state) {
        auto state = movement::DeserializeMovementState(r);
        if (!state)
          return Error{ErrorCode::kInvalidArgument, "OffloadEntity: movement state truncated"};
        msg.movement_state = *state;
      }
    }
    if (r.Remaining() >= 1) {
      auto history_count = r.ReadPackedInt();
      if (!history_count) {
        return Error{ErrorCode::kInvalidArgument,
                     "OffloadEntity: movement history count truncated"};
      }
      if (*history_count > MovementPositionHistoryStore::kDefaultMaxSamplesPerEntity) {
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: movement history too large"};
      }
      msg.movement_position_history.reserve(*history_count);
      for (uint32_t i = 0; i < *history_count; ++i) {
        auto tick = r.Read<uint32_t>();
        if (!tick) {
          return Error{ErrorCode::kInvalidArgument,
                       "OffloadEntity: movement history tick truncated"};
        }
        auto state = movement::DeserializeMovementState(r);
        if (!state) {
          return Error{ErrorCode::kInvalidArgument,
                       "OffloadEntity: movement history state truncated"};
        }
        msg.movement_position_history.push_back(MovementPositionSample{*tick, *state});
      }
    }
    if (r.Remaining() >= sizeof(uint8_t)) {
      auto has_command = r.Read<uint8_t>();
      if (!has_command) {
        return Error{ErrorCode::kInvalidArgument,
                     "OffloadEntity: movement command flag truncated"};
      }
      msg.has_movement_command = (*has_command != 0);
      if (msg.has_movement_command) {
        auto command = movement::DeserializeMovementCommand(r);
        if (!command) {
          return Error{ErrorCode::kInvalidArgument,
                       "OffloadEntity: movement command truncated"};
        }
        msg.movement_command = *command;
      }
    }
    if (r.Remaining() >= sizeof(uint8_t)) {
      auto teleport = r.Read<uint8_t>();
      if (!teleport)
        return Error{ErrorCode::kInvalidArgument, "OffloadEntity: is_teleport truncated"};
      msg.is_teleport = (*teleport != 0);
    }
    return msg;
  }
};
static_assert(NetworkMessage<OffloadEntity>);

enum class OffloadRejectReason : uint8_t {
  kNone = 0,
  kRejected = 1,
  kStaleGeometry = 2,
  kTargetMissing = 3,
  kRestoreFailed = 4,
};

struct OffloadEntityAck {
  EntityID entity_id{kInvalidEntityID};
  bool success{false};
  OffloadRejectReason reject_reason{OffloadRejectReason::kRejected};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kOffloadEntityAck),
                                   "cellapp::OffloadEntityAck",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t) +
                                                    sizeof(uint8_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(entity_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    const auto reason = success ? OffloadRejectReason::kNone : reject_reason;
    w.Write(static_cast<uint8_t>(reason));
  }

  static auto Deserialize(BinaryReader& r) -> Result<OffloadEntityAck> {
    auto eid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto reason = r.Read<uint8_t>();
    if (!eid || !ok || !reason)
      return Error{ErrorCode::kInvalidArgument, "OffloadEntityAck: truncated"};
    OffloadEntityAck msg;
    msg.entity_id = *eid;
    msg.success = (*ok != 0);
    if (*reason > static_cast<uint8_t>(OffloadRejectReason::kRestoreFailed)) {
      return Error{ErrorCode::kInvalidArgument, "OffloadEntityAck: bad reason"};
    }
    msg.reject_reason = static_cast<OffloadRejectReason>(*reason);
    return msg;
  }
};
static_assert(NetworkMessage<OffloadEntityAck>);

// Owner-authoritative SpaceData wire: writes fan out from the BSP
// primary-cell cellapp; snapshot seeds a newly-joining cellapp.

struct SpaceDataUpdate {
  SpaceID space_id{kInvalidSpaceID};
  uint16_t key_id{0};
  std::vector<uint8_t> value;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kSpaceDataUpdate),
                                   "cellapp::SpaceDataUpdate",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(key_id);
    w.WritePackedInt(static_cast<uint32_t>(value.size()));
    if (!value.empty())
      w.WriteBytes(std::as_bytes(std::span<const uint8_t>(value.data(), value.size())));
  }

  static auto Deserialize(BinaryReader& r) -> Result<SpaceDataUpdate> {
    auto sid = r.Read<uint32_t>();
    auto kid = r.Read<uint16_t>();
    auto vlen = r.ReadPackedInt();
    if (!sid || !kid || !vlen)
      return Error{ErrorCode::kInvalidArgument, "SpaceDataUpdate: truncated"};
    SpaceDataUpdate msg;
    msg.space_id = *sid;
    msg.key_id = *kid;
    if (*vlen > 0) {
      auto bytes = r.ReadBytes(*vlen);
      if (!bytes) return Error{ErrorCode::kInvalidArgument, "SpaceDataUpdate: value truncated"};
      msg.value.assign(reinterpret_cast<const uint8_t*>(bytes->data()),
                       reinterpret_cast<const uint8_t*>(bytes->data()) + bytes->size());
    }
    return msg;
  }
};
static_assert(NetworkMessage<SpaceDataUpdate>);

struct SpaceDataDelete {
  SpaceID space_id{kInvalidSpaceID};
  uint16_t key_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kSpaceDataDelete),
                                   "cellapp::SpaceDataDelete",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kBatched};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(key_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<SpaceDataDelete> {
    auto sid = r.Read<uint32_t>();
    auto kid = r.Read<uint16_t>();
    if (!sid || !kid) return Error{ErrorCode::kInvalidArgument, "SpaceDataDelete: truncated"};
    SpaceDataDelete msg;
    msg.space_id = *sid;
    msg.key_id = *kid;
    return msg;
  }
};
static_assert(NetworkMessage<SpaceDataDelete>);

struct SpaceDataSnapshotRequest {
  SpaceID space_id{kInvalidSpaceID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kSpaceDataSnapshotRequest),
                                   "cellapp::SpaceDataSnapshotRequest",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(space_id); }

  static auto Deserialize(BinaryReader& r) -> Result<SpaceDataSnapshotRequest> {
    auto sid = r.Read<uint32_t>();
    if (!sid) return Error{ErrorCode::kInvalidArgument, "SpaceDataSnapshotRequest: truncated"};
    SpaceDataSnapshotRequest msg;
    msg.space_id = *sid;
    return msg;
  }
};
static_assert(NetworkMessage<SpaceDataSnapshotRequest>);

struct SpaceDataSnapshot {
  SpaceID space_id{kInvalidSpaceID};
  struct Entry {
    uint16_t key_id{0};
    std::vector<uint8_t> value;
  };
  std::vector<Entry> entries;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellApp::kSpaceDataSnapshot),
                                   "cellapp::SpaceDataSnapshot",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.WritePackedInt(static_cast<uint32_t>(entries.size()));
    for (const auto& e : entries) {
      w.Write(e.key_id);
      w.WritePackedInt(static_cast<uint32_t>(e.value.size()));
      if (!e.value.empty())
        w.WriteBytes(std::as_bytes(std::span<const uint8_t>(e.value.data(), e.value.size())));
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<SpaceDataSnapshot> {
    auto sid = r.Read<uint32_t>();
    auto count = r.ReadPackedInt();
    if (!sid || !count) return Error{ErrorCode::kInvalidArgument, "SpaceDataSnapshot: truncated"};
    SpaceDataSnapshot msg;
    msg.space_id = *sid;
    msg.entries.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto kid = r.Read<uint16_t>();
      auto vlen = r.ReadPackedInt();
      if (!kid || !vlen)
        return Error{ErrorCode::kInvalidArgument, "SpaceDataSnapshot: entry truncated"};
      Entry e;
      e.key_id = *kid;
      if (*vlen > 0) {
        auto bytes = r.ReadBytes(*vlen);
        if (!bytes)
          return Error{ErrorCode::kInvalidArgument, "SpaceDataSnapshot: value truncated"};
        e.value.assign(reinterpret_cast<const uint8_t*>(bytes->data()),
                       reinterpret_cast<const uint8_t*>(bytes->data()) + bytes->size());
      }
      msg.entries.push_back(std::move(e));
    }
    return msg;
  }
};
static_assert(NetworkMessage<SpaceDataSnapshot>);

}  // namespace atlas::cellapp

#endif  // ATLAS_SERVER_CELLAPP_INTERCELL_MESSAGES_H_
