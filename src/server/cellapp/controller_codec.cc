#include "controller_codec.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <unordered_set>

#include "cell_entity.h"
#include "foundation/log.h"
#include "space.h"
#include "space/controller.h"
#include "space/controllers.h"
#include "space/entity_range_list_node.h"
#include "space/move_controller.h"
#include "space/proximity_controller.h"
#include "space/range_list.h"
#include "space/timer_controller.h"

namespace atlas {

namespace {

void WriteU8(BinaryWriter& w, uint8_t v) {
  w.Write(v);
}
void WriteU32(BinaryWriter& w, uint32_t v) {
  w.Write(v);
}
void WriteI32(BinaryWriter& w, int32_t v) {
  w.Write(v);
}
void WriteF32(BinaryWriter& w, float v) {
  w.Write(v);
}

auto ReadU8(BinaryReader& r) -> Result<uint8_t> {
  return r.Read<uint8_t>();
}
auto ReadU32(BinaryReader& r) -> Result<uint32_t> {
  return r.Read<uint32_t>();
}
auto ReadI32(BinaryReader& r) -> Result<int32_t> {
  return r.Read<int32_t>();
}
auto ReadF32(BinaryReader& r) -> Result<float> {
  return r.Read<float>();
}

void EncodeMoveToPoint(const MoveToPointController& c, BinaryWriter& w) {
  WriteF32(w, c.Destination().x);
  WriteF32(w, c.Destination().y);
  WriteF32(w, c.Destination().z);
  WriteF32(w, c.Speed());
  WriteU8(w, c.FaceMovement() ? 1 : 0);
}

void EncodeTimer(const TimerController& c, BinaryWriter& w) {
  WriteF32(w, c.Interval());
  WriteU8(w, c.Repeat() ? 1 : 0);
  WriteF32(w, c.Accumulated());
  WriteU32(w, c.FireCount());
}

void EncodeProximity(const ProximityController& c, BinaryWriter& w) {
  WriteF32(w, c.Range());
  const auto& peers = c.InsidePeers();
  // Two-pass: defensive OwnerData check may filter some peers.
  std::vector<uint32_t> peer_ids;
  peer_ids.reserve(peers.size());
  for (auto* peer_node : peers) {
    if (peer_node == nullptr) continue;
    // ProximityController inside_peers_ contains only entity centrals
    // (RangeTrigger filters bound nodes), which are always EntityRangeListNode.
    auto* entity_node = static_cast<EntityRangeListNode*>(peer_node);
    auto* peer_entity = static_cast<CellEntity*>(entity_node->OwnerData());
    if (peer_entity == nullptr) continue;
    peer_ids.push_back(peer_entity->Id());
  }
  w.WritePackedInt(static_cast<uint32_t>(peer_ids.size()));
  for (auto id : peer_ids) WriteU32(w, id);
}

auto DecodeMoveToPoint(BinaryReader& r) -> std::unique_ptr<Controller> {
  auto dx = ReadF32(r);
  auto dy = ReadF32(r);
  auto dz = ReadF32(r);
  auto spd = ReadF32(r);
  auto fm = ReadU8(r);
  if (!dx || !dy || !dz || !spd || !fm) return nullptr;
  return std::make_unique<MoveToPointController>(math::Vector3{*dx, *dy, *dz}, *spd, *fm != 0);
}

auto DecodeTimer(BinaryReader& r) -> std::unique_ptr<Controller> {
  auto interval = ReadF32(r);
  auto repeat = ReadU8(r);
  auto accum = ReadF32(r);
  auto fcount = ReadU32(r);
  if (!interval || !repeat || !accum || !fcount) return nullptr;
  auto timer =
      std::make_unique<TimerController>(*interval, *repeat != 0, TimerController::FireFn{});
  timer->RestoreRunningStateForMigration(*accum, *fcount);
  return timer;
}

// Proximity decode is inline in the caller - needs the owner's RangeNode
// + Space's RangeList + lookup_peer.

}  // namespace

// Wire: u32 count | per-ctrl (u8 kind, u32 id, i32 user_arg, payload).
// lastAllocatedID is not on the wire; AddWithPreservedId advances
// next_id_ per restore so next_id_ ends at max(preserved_ids)+1.
void SerializeControllersForMigration(const CellEntity& entity, BinaryWriter& w) {
  const auto& controllers = const_cast<CellEntity&>(entity).GetControllers();
  // Two-pass; ForEach order is consistent within a single CellApp run.
  uint32_t count = 0;
  controllers.ForEach([&](const Controller& c) {
    if (c.TypeTag() == ControllerKind::kUnknown) return;
    if (c.IsFinished()) return;
    ++count;
  });
  WriteU32(w, count);
  controllers.ForEach([&](const Controller& c) {
    const auto kind = c.TypeTag();
    if (kind == ControllerKind::kUnknown) return;
    if (c.IsFinished()) return;
    WriteU8(w, static_cast<uint8_t>(kind));
    WriteU32(w, c.Id());
    WriteI32(w, c.UserArg());
    switch (kind) {
      case ControllerKind::kMoveToPoint:
        EncodeMoveToPoint(static_cast<const MoveToPointController&>(c), w);
        break;
      case ControllerKind::kTimer:
        EncodeTimer(static_cast<const TimerController&>(c), w);
        break;
      case ControllerKind::kProximity:
        EncodeProximity(static_cast<const ProximityController&>(c), w);
        break;
      case ControllerKind::kUnknown:
        break;
    }
  });
}

auto DeserializeControllersForMigration(CellEntity& entity, BinaryReader& r,
                                        const EntityLookupByIdFn& lookup_peer) -> bool {
  auto count = ReadU32(r);
  if (!count) return false;
  auto& controllers = entity.GetControllers();
  for (uint32_t i = 0; i < *count; ++i) {
    auto kind_raw = ReadU8(r);
    auto id = ReadU32(r);
    auto user_arg = ReadI32(r);
    if (!kind_raw || !id || !user_arg) return false;
    const auto kind = static_cast<ControllerKind>(*kind_raw);

    std::unique_ptr<Controller> ctrl;
    std::unordered_set<RangeListNode*> prox_seed;

    switch (kind) {
      case ControllerKind::kMoveToPoint:
        ctrl = DecodeMoveToPoint(r);
        break;
      case ControllerKind::kTimer:
        ctrl = DecodeTimer(r);
        break;
      case ControllerKind::kProximity: {
        auto range = ReadF32(r);
        auto peer_count = r.ReadPackedInt();
        if (!range || !peer_count) return false;
        prox_seed.reserve(*peer_count);
        for (uint32_t p = 0; p < *peer_count; ++p) {
          auto peer_id = ReadU32(r);
          if (!peer_id) return false;
          // Cross-partition peers drop silently; trigger Insert can't
          // fire events for nodes not in the list.
          if (lookup_peer) {
            if (auto* peer_entity = lookup_peer(*peer_id)) {
              prox_seed.insert(&peer_entity->RangeNode());
            }
          }
        }
        auto prox = std::make_unique<ProximityController>(
            entity.RangeNode(), entity.GetSpace().GetRangeList(), *range,
            ProximityController::EnterFn{}, ProximityController::LeaveFn{});
        prox->SeedInsidePeersForMigration(std::move(prox_seed));
        ctrl = std::move(prox);
        break;
      }
      case ControllerKind::kUnknown:
      default:
        ATLAS_LOG_ERROR("ControllerCodec: unknown Kind tag {} — aborting restore at index {}",
                        *kind_raw, i);
        return false;
    }
    if (ctrl == nullptr) {
      ATLAS_LOG_ERROR("ControllerCodec: decode failed for Kind {} at index {}", *kind_raw, i);
      return false;
    }

    // Only MoveToPoint needs the motion surface (matches CellAppNativeProvider).
    IEntityMotion* motion = (kind == ControllerKind::kMoveToPoint) ? &entity : nullptr;
    const auto assigned = controllers.AddWithPreservedId(std::move(ctrl), motion, *user_arg, *id);
    if (assigned == 0) {
      ATLAS_LOG_WARNING("ControllerCodec: preserved id {} already in use — skipping", *id);
    }
  }
  return true;
}

}  // namespace atlas
