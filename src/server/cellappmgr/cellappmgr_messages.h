#ifndef ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_MESSAGES_H_
#define ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_MESSAGES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cellapp/cell_bounds.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"
#include "server/entity_types.h"

// BSP tree geometry moves between CellAppMgr and CellApps as a
// pre-serialised blob rather than typed fields; the mgr is
// authoritative for the tree and CellApp only consumes it.

namespace atlas::cellappmgr {

using CellID = uint32_t;

struct RegisterCellApp {
  Address internal_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kRegisterCellApp),
                                   "cellappmgr::RegisterCellApp",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(internal_addr.Ip());
    w.Write(internal_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterCellApp> {
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    if (!ip || !port) return Error{ErrorCode::kInvalidArgument, "RegisterCellApp: truncated"};
    RegisterCellApp msg;
    msg.internal_addr = Address(*ip, *port);
    return msg;
  }
};
static_assert(NetworkMessage<RegisterCellApp>);

// The assigned `app_id` drives EntityID allocation: high 8 bits of the
// EntityID are app_id, low 24 bits are CellApp-local monotonic.
// Consequently app_id must be in [1, 255]; app_id == 0 is reserved
// (matches kInvalidEntityID's prefix).

struct RegisterCellAppAck {
  bool success{false};
  uint32_t app_id{0};
  uint64_t game_time{0};
  // Cluster-wide tick reference (mgr's monotonic μs); cellapps phase
  // their next tick boundary to it so Ghost updates land aligned.
  uint64_t tick_alignment_epoch_us{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellAppMgr::kRegisterCellAppAck),
        "cellappmgr::RegisterCellAppAck",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint8_t) + sizeof(uint32_t) + 2 * sizeof(uint64_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(app_id);
    w.Write(game_time);
    w.Write(tick_alignment_epoch_us);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterCellAppAck> {
    auto ok = r.Read<uint8_t>();
    auto aid = r.Read<uint32_t>();
    auto gt = r.Read<uint64_t>();
    auto tep = r.Read<uint64_t>();
    if (!ok || !aid || !gt || !tep)
      return Error{ErrorCode::kInvalidArgument, "RegisterCellAppAck: truncated"};
    RegisterCellAppAck msg;
    msg.success = (*ok != 0);
    msg.app_id = *aid;
    msg.game_time = *gt;
    msg.tick_alignment_epoch_us = *tep;
    return msg;
  }
};
static_assert(NetworkMessage<RegisterCellAppAck>);

struct InformCellLoad {
  uint32_t app_id{0};
  float load{0.0f};
  uint32_t entity_count{0};
  // Per-cell load profile; mgr uses it for weighted LB scoring and Split placement.
  struct CellReport {
    static constexpr std::size_t kLoadBucketCount = 8;

    CellID cell_id{0};
    uint32_t entity_count{0};
    float median_x{0.f};
    float median_z{0.f};
    uint64_t geometry_version{0};
    float tick_load{0.f};
    uint64_t script_tick_us{0};
    uint32_t witness_count{0};
    uint32_t aoi_peer_count{0};
    uint64_t aoi_reliable_bytes{0};
    uint64_t aoi_unreliable_bytes{0};
    uint64_t backup_bytes{0};
    std::array<uint32_t, kLoadBucketCount> x_buckets{};
    std::array<uint32_t, kLoadBucketCount> z_buckets{};
  };
  std::vector<CellReport> cells;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kInformCellLoad),
                                   "cellappmgr::InformCellLoad",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(app_id);
    w.Write(load);
    w.Write(entity_count);
    w.WritePackedInt(static_cast<uint32_t>(cells.size()));
    for (const auto& c : cells) {
      w.Write(c.cell_id);
      w.Write(c.entity_count);
      w.Write(c.median_x);
      w.Write(c.median_z);
      w.Write(c.geometry_version);
      w.Write(c.tick_load);
      w.Write(c.script_tick_us);
      w.Write(c.witness_count);
      w.Write(c.aoi_peer_count);
      w.Write(c.aoi_reliable_bytes);
      w.Write(c.aoi_unreliable_bytes);
      w.Write(c.backup_bytes);
      for (uint32_t bucket : c.x_buckets) w.Write(bucket);
      for (uint32_t bucket : c.z_buckets) w.Write(bucket);
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<InformCellLoad> {
    auto aid = r.Read<uint32_t>();
    auto ld = r.Read<float>();
    auto ec = r.Read<uint32_t>();
    auto count = r.ReadPackedInt();
    if (!aid || !ld || !ec || !count)
      return Error{ErrorCode::kInvalidArgument, "InformCellLoad: truncated"};
    InformCellLoad msg;
    msg.app_id = *aid;
    msg.load = *ld;
    msg.entity_count = *ec;
    msg.cells.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto cid = r.Read<uint32_t>();
      auto ce = r.Read<uint32_t>();
      auto mx = r.Read<float>();
      auto mz = r.Read<float>();
      auto gv = r.Read<uint64_t>();
      auto tl = r.Read<float>();
      auto stu = r.Read<uint64_t>();
      auto wc = r.Read<uint32_t>();
      auto ap = r.Read<uint32_t>();
      auto arb = r.Read<uint64_t>();
      auto aub = r.Read<uint64_t>();
      auto bb = r.Read<uint64_t>();
      if (!cid || !ce || !mx || !mz || !gv || !tl || !stu || !wc || !ap || !arb || !aub || !bb)
        return Error{ErrorCode::kInvalidArgument, "InformCellLoad: cell entry truncated"};
      CellReport rep;
      rep.cell_id = *cid;
      rep.entity_count = *ce;
      rep.median_x = *mx;
      rep.median_z = *mz;
      rep.geometry_version = *gv;
      rep.tick_load = *tl;
      rep.script_tick_us = *stu;
      rep.witness_count = *wc;
      rep.aoi_peer_count = *ap;
      rep.aoi_reliable_bytes = *arb;
      rep.aoi_unreliable_bytes = *aub;
      rep.backup_bytes = *bb;
      for (auto& bucket : rep.x_buckets) {
        auto value = r.Read<uint32_t>();
        if (!value)
          return Error{ErrorCode::kInvalidArgument, "InformCellLoad: x bucket truncated"};
        bucket = *value;
      }
      for (auto& bucket : rep.z_buckets) {
        auto value = r.Read<uint32_t>();
        if (!value)
          return Error{ErrorCode::kInvalidArgument, "InformCellLoad: z bucket truncated"};
        bucket = *value;
      }
      msg.cells.push_back(rep);
    }
    return msg;
  }
};
static_assert(NetworkMessage<InformCellLoad>);

struct RequestCellAppState {
  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kRequestCellAppState),
                                   "cellappmgr::RequestCellAppState",
                                   MessageLengthStyle::kFixed,
                                   0,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter&) const {}

  static auto Deserialize(BinaryReader&) -> Result<RequestCellAppState> {
    return RequestCellAppState{};
  }
};
static_assert(NetworkMessage<RequestCellAppState>);

struct CreateSpaceRequest {
  SpaceID space_id{kInvalidSpaceID};
  uint32_t request_id{0};
  Address reply_addr;
  // 1 = legacy single-cell. >1 = bootstrap N cells distributed across the
  // N least-loaded registered CellApps; ignored when fewer cellapps exist.
  uint16_t initial_cell_count{1};
  // Empty = no space-owner entity; non-empty resolves via CellApp's EntityDef
  // registry on the primary host and is auto-spawned at AddCellToSpace time.
  std::string space_master_type;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kCreateSpaceRequest),
                                   "cellappmgr::CreateSpaceRequest",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(request_id);
    w.Write(reply_addr.Ip());
    w.Write(reply_addr.Port());
    w.Write(initial_cell_count);
    w.WriteString(space_master_type);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CreateSpaceRequest> {
    auto sid = r.Read<uint32_t>();
    auto rid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto cc = r.Read<uint16_t>();
    auto mt = r.ReadString();
    if (!sid || !rid || !ip || !port || !cc || !mt)
      return Error{ErrorCode::kInvalidArgument, "CreateSpaceRequest: truncated"};
    CreateSpaceRequest msg;
    msg.space_id = *sid;
    msg.request_id = *rid;
    msg.reply_addr = Address(*ip, *port);
    msg.initial_cell_count = *cc;
    msg.space_master_type = std::move(*mt);
    return msg;
  }
};
static_assert(NetworkMessage<CreateSpaceRequest>);

struct AddCellToSpace {
  SpaceID space_id{kInvalidSpaceID};
  CellID cell_id{0};
  CellBounds bounds;
  // True on the cell hosting the BSP's primary leaf; the cellapp spawns the
  // space-owner entity at this moment when space_master_type is non-empty.
  bool is_primary{false};
  std::string space_master_type;
  Address space_data_source_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kAddCellToSpace),
                                   "cellappmgr::AddCellToSpace",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(cell_id);
    bounds.Serialize(w);
    w.Write<uint8_t>(is_primary ? 1u : 0u);
    w.WriteString(space_master_type);
    w.Write(space_data_source_addr.Ip());
    w.Write(space_data_source_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<AddCellToSpace> {
    auto sid = r.Read<uint32_t>();
    auto cid = r.Read<uint32_t>();
    if (!sid || !cid) return Error{ErrorCode::kInvalidArgument, "AddCellToSpace: truncated"};
    auto b = CellBounds::Deserialize(r);
    if (!b) return b.Error();
    auto p = r.Read<uint8_t>();
    auto mt = r.ReadString();
    if (!p || !mt) return Error{ErrorCode::kInvalidArgument, "AddCellToSpace: truncated"};
    AddCellToSpace msg;
    msg.space_id = *sid;
    msg.cell_id = *cid;
    msg.bounds = *b;
    msg.is_primary = *p != 0;
    msg.space_master_type = std::move(*mt);
    if (r.Remaining() > 0) {
      auto ip = r.Read<uint32_t>();
      auto port = r.Read<uint16_t>();
      if (!ip || !port) {
        return Error{ErrorCode::kInvalidArgument, "AddCellToSpace: truncated"};
      }
      msg.space_data_source_addr = Address(*ip, *port);
    }
    return msg;
  }
};
static_assert(NetworkMessage<AddCellToSpace>);

// Signals the local Cell is in place; CellAppMgr holds UpdateGeometry until
// this lands so old cellapps don't offload into a missing cell.
struct AddCellToSpaceAck {
  SpaceID space_id{kInvalidSpaceID};
  CellID cell_id{0};
  bool success{true};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kAddCellToSpaceAck),
                                   "cellappmgr::AddCellToSpaceAck",
                                   MessageLengthStyle::kFixed,
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(cell_id);
    w.Write<uint8_t>(success ? 1u : 0u);
  }

  static auto Deserialize(BinaryReader& r) -> Result<AddCellToSpaceAck> {
    auto sid = r.Read<uint32_t>();
    auto cid = r.Read<uint32_t>();
    auto s = r.Read<uint8_t>();
    if (!sid || !cid || !s)
      return Error{ErrorCode::kInvalidArgument, "AddCellToSpaceAck: truncated"};
    AddCellToSpaceAck msg;
    msg.space_id = *sid;
    msg.cell_id = *cid;
    msg.success = *s != 0;
    return msg;
  }
};
static_assert(NetworkMessage<AddCellToSpaceAck>);

struct RemoveCellFromSpace {
  SpaceID space_id{kInvalidSpaceID};
  CellID cell_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kRemoveCellFromSpace),
                                   "cellappmgr::RemoveCellFromSpace",
                                   MessageLengthStyle::kFixed,
                                   sizeof(uint32_t) + sizeof(uint32_t),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(cell_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RemoveCellFromSpace> {
    auto sid = r.Read<uint32_t>();
    auto cid = r.Read<uint32_t>();
    if (!sid || !cid)
      return Error{ErrorCode::kInvalidArgument, "RemoveCellFromSpace: truncated"};
    RemoveCellFromSpace msg;
    msg.space_id = *sid;
    msg.cell_id = *cid;
    return msg;
  }
};
static_assert(NetworkMessage<RemoveCellFromSpace>);

// `bsp_blob` is the BSPTree serialization; the structure itself is
// defined in src/server/cellappmgr/bsp_tree.h and opaque to the
// message layer - CellAppMgr owns the tree, CellApp replays it.

struct UpdateGeometry {
  SpaceID space_id{kInvalidSpaceID};
  uint64_t geometry_version{0};
  std::vector<std::byte> bsp_blob;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kUpdateGeometry),
                                   "cellappmgr::UpdateGeometry",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(geometry_version);
    w.WritePackedInt(static_cast<uint32_t>(bsp_blob.size()));
    if (!bsp_blob.empty()) w.WriteBytes(std::span<const std::byte>(bsp_blob));
  }

  static auto Deserialize(BinaryReader& r) -> Result<UpdateGeometry> {
    auto sid = r.Read<uint32_t>();
    auto version = r.Read<uint64_t>();
    auto blen = r.ReadPackedInt();
    if (!sid || !version || !blen)
      return Error{ErrorCode::kInvalidArgument, "UpdateGeometry: truncated"};
    UpdateGeometry msg;
    msg.space_id = *sid;
    msg.geometry_version = *version;
    if (*blen > 0) {
      auto data = r.ReadBytes(*blen);
      if (!data) return Error{ErrorCode::kInvalidArgument, "UpdateGeometry: bsp_blob truncated"};
      msg.bsp_blob.assign(data->begin(), data->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<UpdateGeometry>);

// Enables / disables entity migration for a Cell. CellAppMgr toggles
// this off during sensitive transitions (e.g. a BSP rebalance is in
// flight) and re-enables once the new geometry has quiesced.

struct ShouldOffload {
  SpaceID space_id{kInvalidSpaceID};
  CellID cell_id{0};
  bool enable{true};
  uint64_t freeze_epoch{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kShouldOffload),
                                   "cellappmgr::ShouldOffload",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) * 2 + sizeof(uint8_t) +
                                                    sizeof(uint64_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(cell_id);
    w.Write(static_cast<uint8_t>(enable ? 1 : 0));
    w.Write(freeze_epoch);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ShouldOffload> {
    auto sid = r.Read<uint32_t>();
    auto cid = r.Read<uint32_t>();
    auto en = r.Read<uint8_t>();
    auto epoch = r.Read<uint64_t>();
    if (!sid || !cid || !en || !epoch)
      return Error{ErrorCode::kInvalidArgument, "ShouldOffload: truncated"};
    ShouldOffload msg;
    msg.space_id = *sid;
    msg.cell_id = *cid;
    msg.enable = (*en != 0);
    msg.freeze_epoch = *epoch;
    return msg;
  }
};
static_assert(NetworkMessage<ShouldOffload>);

// CellAppMgr replies to every CreateSpaceRequest with this message so
// the originating BaseApp (or script) can resolve its per-request
// callback and learn which CellApp now hosts the initial Cell of the
// Space. Failure cases carry success=false and zeroed host_addr /
// cell_id; the caller should treat them as "no Space exists" and
// retry / surface the error to game code.

struct SpaceCreatedResult {
  uint32_t request_id{0};
  SpaceID space_id{kInvalidSpaceID};
  bool success{false};
  CellID cell_id{0};
  Address host_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellAppMgr::kSpaceCreatedResult),
        "cellappmgr::SpaceCreatedResult",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) +
                         sizeof(uint32_t) + sizeof(uint16_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(space_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(cell_id);
    w.Write(host_addr.Ip());
    w.Write(host_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<SpaceCreatedResult> {
    auto rid = r.Read<uint32_t>();
    auto sid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto cid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    if (!rid || !sid || !ok || !cid || !ip || !port)
      return Error{ErrorCode::kInvalidArgument, "SpaceCreatedResult: truncated"};
    SpaceCreatedResult msg;
    msg.request_id = *rid;
    msg.space_id = *sid;
    msg.success = (*ok != 0);
    msg.cell_id = *cid;
    msg.host_addr = Address(*ip, *port);
    return msg;
  }
};
static_assert(NetworkMessage<SpaceCreatedResult>);

}  // namespace atlas::cellappmgr

#endif  // ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_MESSAGES_H_
