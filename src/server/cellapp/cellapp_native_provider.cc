#include "cellapp_native_provider.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "cell_entity.h"
#include "foundation/log.h"
#include "math/vector3.h"
#include "server/server_config.h"
#include "space.h"
#include "space/move_controller.h"
#include "space/proximity_controller.h"
#include "space/timer_controller.h"

namespace atlas {

// Packed layout of the callback table sent by C# Atlas.Runtime via
// set_native_callbacks. Must match the [UnmanagedCallersOnly] exports in
// Atlas.Runtime. Identical struct as BaseAppNativeProvider uses — the C#
// side fills the same table regardless of process type.
#pragma pack(push, 1)
struct CellAppCallbackTable {
  RestoreEntityFn restore_entity;
  GetEntityDataFn get_entity_data;
  EntityDestroyedFn entity_destroyed;
  DispatchRpcFn dispatch_rpc;
  GetOwnerSnapshotFn get_owner_snapshot;
};
#pragma pack(pop)

CellAppNativeProvider::CellAppNativeProvider(EntityLookupFn lookup) : lookup_(std::move(lookup)) {}

uint8_t CellAppNativeProvider::GetProcessPrefix() {
  return static_cast<uint8_t>(ProcessType::kCellApp);
}

void CellAppNativeProvider::SetEntityPosition(uint32_t entity_id, float x, float y, float z) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_set_position: unknown entity_id={}", entity_id);
    return;
  }
  // SetPosition handles the RangeList shuffle; the MarkVolatileDirty on
  // the C# side happens when the script property setter runs, so here we
  // just propagate the coordinate change. (The C#-layer dirty flip is
  // what ultimately advances VolatileSeq in the next replication frame.)
  entity->SetPosition(math::Vector3{x, y, z});
}

void CellAppNativeProvider::PublishReplicationFrame(
    uint32_t entity_id, uint64_t event_seq, uint64_t volatile_seq, const std::byte* owner_snap,
    int32_t owner_snap_len, const std::byte* other_snap, int32_t other_snap_len,
    const std::byte* owner_delta, int32_t owner_delta_len, const std::byte* other_delta,
    int32_t other_delta_len) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_publish_replication_frame: unknown entity_id={}", entity_id);
    return;
  }

  CellEntity::ReplicationFrame frame;
  frame.event_seq = event_seq;
  frame.volatile_seq = volatile_seq;
  if (owner_delta_len > 0 && owner_delta != nullptr) {
    frame.owner_delta.assign(owner_delta, owner_delta + owner_delta_len);
  }
  if (other_delta_len > 0 && other_delta != nullptr) {
    frame.other_delta.assign(other_delta, other_delta + other_delta_len);
  }
  // The C# side passes frame position/direction implicitly via the prior
  // SetEntityPosition call; here we just adopt the entity's current
  // position so PublishReplicationFrame's volatile branch doesn't
  // overwrite it with stale zeros.
  frame.position = entity->Position();
  frame.direction = entity->Direction();
  frame.on_ground = entity->OnGround();

  std::span<const std::byte> owner_snap_span{};
  std::span<const std::byte> other_snap_span{};
  if (owner_snap != nullptr && owner_snap_len > 0) {
    owner_snap_span = std::span<const std::byte>(owner_snap, owner_snap_len);
  }
  if (other_snap != nullptr && other_snap_len > 0) {
    other_snap_span = std::span<const std::byte>(other_snap, other_snap_len);
  }

  entity->PublishReplicationFrame(std::move(frame), owner_snap_span, other_snap_span);
}

auto CellAppNativeProvider::AddMoveController(uint32_t entity_id, float dest_x, float dest_y,
                                              float dest_z, float speed, int32_t user_arg)
    -> int32_t {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_add_move_controller: unknown entity_id={}", entity_id);
    return 0;
  }
  return static_cast<int32_t>(entity->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{dest_x, dest_y, dest_z}, speed,
                                              /*face_movement=*/false),
      /*motion=*/entity, user_arg));
}

auto CellAppNativeProvider::AddTimerController(uint32_t entity_id, float interval, bool repeat,
                                               int32_t user_arg) -> int32_t {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_add_timer_controller: unknown entity_id={}", entity_id);
    return 0;
  }
  return static_cast<int32_t>(
      entity->GetControllers().Add(std::make_unique<TimerController>(interval, repeat),
                                   /*motion=*/nullptr, user_arg));
}

auto CellAppNativeProvider::AddProximityController(uint32_t entity_id, float range,
                                                   int32_t user_arg) -> int32_t {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_add_proximity_controller: unknown entity_id={}", entity_id);
    return 0;
  }
  // The controller callbacks are stubbed for now — Step 10.8's CellApp
  // process hooks them into a C# callback dispatcher so scripts can
  // react to proximity events. For Step 10.7's skeleton the range
  // trigger is functional but fires into no-op lambdas.
  return static_cast<int32_t>(entity->GetControllers().Add(
      std::make_unique<ProximityController>(entity->RangeNode(), entity->GetSpace().GetRangeList(),
                                            range,
                                            /*on_enter=*/nullptr, /*on_leave=*/nullptr),
      /*motion=*/nullptr, user_arg));
}

void CellAppNativeProvider::CancelController(uint32_t entity_id, int32_t controller_id) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_cancel_controller: unknown entity_id={}", entity_id);
    return;
  }
  entity->GetControllers().Cancel(static_cast<ControllerID>(controller_id));
}

void CellAppNativeProvider::SetNativeCallbacks(const void* native_callbacks, int32_t len) {
  // Minimum = 4 legacy entries (restore + get_data + destroyed + dispatch).
  constexpr int32_t kMinTableBytes =
      static_cast<int32_t>(sizeof(RestoreEntityFn) + sizeof(GetEntityDataFn) +
                           sizeof(EntityDestroyedFn) + sizeof(DispatchRpcFn));
  if (!native_callbacks || len < kMinTableBytes) {
    ATLAS_LOG_ERROR("CellApp: set_native_callbacks: invalid callback table (len={})", len);
    return;
  }

  CellAppCallbackTable table{};
  const auto copy_bytes =
      std::min<int32_t>(len, static_cast<int32_t>(sizeof(CellAppCallbackTable)));
  std::memcpy(&table, native_callbacks, static_cast<size_t>(copy_bytes));

  restore_entity_fn_ = table.restore_entity;
  dispatch_rpc_fn_ = table.dispatch_rpc;
  entity_destroyed_fn_ = table.entity_destroyed;
  // get_entity_data and get_owner_snapshot are not needed on CellApp (no DB
  // persistence or baseline pump), but we accept the table silently.
  ATLAS_LOG_INFO("CellApp: native callback table registered (len={})", len);
}

}  // namespace atlas
