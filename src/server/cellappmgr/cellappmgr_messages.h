#ifndef ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_MESSAGES_H_
#define ATLAS_SERVER_CELLAPPMGR_CELLAPPMGR_MESSAGES_H_

#include <cstdint>
#include <vector>

#include "cellapp/cell_bounds.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"
#include "server/entity_types.h"

// CellAppMgr messages (IDs 7000-7099).
//
// Directions:
//   CellApp    → CellAppMgr : RegisterCellApp, InformCellLoad
//   CellAppMgr → CellApp    : RegisterCellAppAck, AddCellToSpace,
//                              UpdateGeometry, ShouldOffload
//   BaseApp    → CellAppMgr : CreateSpaceRequest
//
// BSP tree geometry moves between CellAppMgr and CellApps as a
// pre-serialised blob rather than typed fields; the mgr is
// authoritative for the tree and CellApp only consumes it.

namespace atlas::cellappmgr {

using CellID = uint32_t;

// RegisterCellApp  (CellApp → CellAppMgr, ID 7000)

struct RegisterCellApp {
  Address internal_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kRegisterCellApp),
                                   "cellappmgr::RegisterCellApp", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t))};
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

// RegisterCellAppAck  (CellAppMgr → CellApp, ID 7001)
//
// The assigned `app_id` drives EntityID allocation: high 8 bits of the
// EntityID are app_id, low 24 bits are CellApp-local monotonic.
// Consequently app_id must be in [1, 255]; app_id == 0 is reserved
// (matches kInvalidEntityID's prefix).

struct RegisterCellAppAck {
  bool success{false};
  uint32_t app_id{0};
  uint64_t game_time{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellAppMgr::kRegisterCellAppAck), "cellappmgr::RegisterCellAppAck",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint64_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(app_id);
    w.Write(game_time);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterCellAppAck> {
    auto ok = r.Read<uint8_t>();
    auto aid = r.Read<uint32_t>();
    auto gt = r.Read<uint64_t>();
    if (!ok || !aid || !gt)
      return Error{ErrorCode::kInvalidArgument, "RegisterCellAppAck: truncated"};
    RegisterCellAppAck msg;
    msg.success = (*ok != 0);
    msg.app_id = *aid;
    msg.game_time = *gt;
    return msg;
  }
};
static_assert(NetworkMessage<RegisterCellAppAck>);

// InformCellLoad  (CellApp → CellAppMgr, ID 7002)

struct InformCellLoad {
  uint32_t app_id{0};
  float load{0.0f};
  uint32_t entity_count{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellAppMgr::kInformCellLoad), "cellappmgr::InformCellLoad",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(float) + sizeof(uint32_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(app_id);
    w.Write(load);
    w.Write(entity_count);
  }

  static auto Deserialize(BinaryReader& r) -> Result<InformCellLoad> {
    auto aid = r.Read<uint32_t>();
    auto ld = r.Read<float>();
    auto ec = r.Read<uint32_t>();
    if (!aid || !ld || !ec) return Error{ErrorCode::kInvalidArgument, "InformCellLoad: truncated"};
    InformCellLoad msg;
    msg.app_id = *aid;
    msg.load = *ld;
    msg.entity_count = *ec;
    return msg;
  }
};
static_assert(NetworkMessage<InformCellLoad>);

// CreateSpaceRequest  (BaseApp / script → CellAppMgr, ID 7003)

struct CreateSpaceRequest {
  SpaceID space_id{kInvalidSpaceID};
  uint32_t request_id{0};
  Address reply_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::CellAppMgr::kCreateSpaceRequest), "cellappmgr::CreateSpaceRequest",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) * 2 + sizeof(uint32_t) + sizeof(uint16_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(request_id);
    w.Write(reply_addr.Ip());
    w.Write(reply_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<CreateSpaceRequest> {
    auto sid = r.Read<uint32_t>();
    auto rid = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    if (!sid || !rid || !ip || !port)
      return Error{ErrorCode::kInvalidArgument, "CreateSpaceRequest: truncated"};
    CreateSpaceRequest msg;
    msg.space_id = *sid;
    msg.request_id = *rid;
    msg.reply_addr = Address(*ip, *port);
    return msg;
  }
};
static_assert(NetworkMessage<CreateSpaceRequest>);

// AddCellToSpace  (CellAppMgr → CellApp, ID 7004)

struct AddCellToSpace {
  SpaceID space_id{kInvalidSpaceID};
  CellID cell_id{0};
  CellBounds bounds;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kAddCellToSpace),
                                   "cellappmgr::AddCellToSpace", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) * 2 + 4 * sizeof(float))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(cell_id);
    bounds.Serialize(w);
  }

  static auto Deserialize(BinaryReader& r) -> Result<AddCellToSpace> {
    auto sid = r.Read<uint32_t>();
    auto cid = r.Read<uint32_t>();
    if (!sid || !cid) return Error{ErrorCode::kInvalidArgument, "AddCellToSpace: truncated"};
    auto b = CellBounds::Deserialize(r);
    if (!b) return b.Error();
    AddCellToSpace msg;
    msg.space_id = *sid;
    msg.cell_id = *cid;
    msg.bounds = *b;
    return msg;
  }
};
static_assert(NetworkMessage<AddCellToSpace>);

// UpdateGeometry  (CellAppMgr → CellApp, ID 7005)
//
// `bsp_blob` is the BSPTree serialization; the structure itself is
// defined in src/server/cellappmgr/bsp_tree.h and opaque to the
// message layer — CellAppMgr owns the tree, CellApp replays it.

struct UpdateGeometry {
  SpaceID space_id{kInvalidSpaceID};
  std::vector<std::byte> bsp_blob;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kUpdateGeometry),
                                   "cellappmgr::UpdateGeometry", MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.WritePackedInt(static_cast<uint32_t>(bsp_blob.size()));
    if (!bsp_blob.empty()) w.WriteBytes(std::span<const std::byte>(bsp_blob));
  }

  static auto Deserialize(BinaryReader& r) -> Result<UpdateGeometry> {
    auto sid = r.Read<uint32_t>();
    auto blen = r.ReadPackedInt();
    if (!sid || !blen) return Error{ErrorCode::kInvalidArgument, "UpdateGeometry: truncated"};
    UpdateGeometry msg;
    msg.space_id = *sid;
    if (*blen > 0) {
      auto data = r.ReadBytes(*blen);
      if (!data) return Error{ErrorCode::kInvalidArgument, "UpdateGeometry: bsp_blob truncated"};
      msg.bsp_blob.assign(data->begin(), data->end());
    }
    return msg;
  }
};
static_assert(NetworkMessage<UpdateGeometry>);

// ShouldOffload  (CellAppMgr → CellApp, ID 7006)
//
// Enables / disables entity migration for a Cell. CellAppMgr toggles
// this off during sensitive transitions (e.g. a BSP rebalance is in
// flight) and re-enables once the new geometry has quiesced.

struct ShouldOffload {
  SpaceID space_id{kInvalidSpaceID};
  CellID cell_id{0};
  bool enable{true};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::CellAppMgr::kShouldOffload),
                                   "cellappmgr::ShouldOffload", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) * 2 + sizeof(uint8_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(space_id);
    w.Write(cell_id);
    w.Write(static_cast<uint8_t>(enable ? 1 : 0));
  }

  static auto Deserialize(BinaryReader& r) -> Result<ShouldOffload> {
    auto sid = r.Read<uint32_t>();
    auto cid = r.Read<uint32_t>();
    auto en = r.Read<uint8_t>();
    if (!sid || !cid || !en) return Error{ErrorCode::kInvalidArgument, "ShouldOffload: truncated"};
    ShouldOffload msg;
    msg.space_id = *sid;
    msg.cell_id = *cid;
    msg.enable = (*en != 0);
    return msg;
  }
};
static_assert(NetworkMessage<ShouldOffload>);

// SpaceCreatedResult  (CellAppMgr → BaseApp, ID 7007)
//
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
        msg_id::Id(msg_id::CellAppMgr::kSpaceCreatedResult), "cellappmgr::SpaceCreatedResult",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) +
                         sizeof(uint32_t) + sizeof(uint16_t))};
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
