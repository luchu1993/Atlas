#include "cellapp_native_provider.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "cell_entity.h"
#include "cellapp_messages.h"
#include "foundation/log.h"
#include "math/vector3.h"
#include "movement_sim/movement_codec.h"
#include "navigation/nav_query.h"
#include "network/channel.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "protocol/aoi_envelope.h"
#include "real_entity_data.h"
#include "server/server_config.h"
#include "space.h"
#include "space/entity_range_list_node.h"
#include "space/move_along_path_controller.h"
#include "space/move_controller.h"
#include "space/proximity_controller.h"
#include "space/timer_controller.h"
#include "witness.h"

namespace atlas {

namespace {

auto IsValidNativePayload(const std::byte* payload, int32_t len) -> bool {
  return len >= 0 && (payload != nullptr || len == 0);
}

auto IsValidNativeString(const char* value, int32_t len) -> bool {
  return len >= 0 && (value != nullptr || len == 0);
}

auto IsValidRpcTarget(RpcTarget target) -> bool {
  return target == RpcTarget::kOwner || target == RpcTarget::kOthers || target == RpcTarget::kAll;
}

auto ToMovementCommand(const NativeMovementCommand& native,
                       movement::MovementCommand& command) -> bool {
  if (!movement::IsMovementCommandTypeWireValue(native.type) ||
      !movement::IsMovementCommandInputPolicyWireValue(native.input_policy) ||
      !movement::IsMovementCommandCollisionPolicyWireValue(native.collision_policy)) {
    return false;
  }

  command.command_id = native.command_id;
  command.skill_id = native.skill_id;
  command.type = static_cast<movement::MovementCommandType>(native.type);
  command.start_position = {native.start_x, native.start_y, native.start_z};
  command.target_position = {native.target_x, native.target_y, native.target_z};
  command.duration_ms = native.duration_ms;
  command.elapsed_ms = native.elapsed_ms;
  command.curve_id = native.curve_id;
  command.input_policy =
      static_cast<movement::MovementCommandInputPolicy>(native.input_policy);
  command.collision_policy =
      static_cast<movement::MovementCommandCollisionPolicy>(native.collision_policy);
  command.priority = native.priority;
  command.server_tick = native.server_tick;
  return true;
}

auto ToMovementCurve(const NativeMovementCurve& native, movement::MovementCurve& curve)
    -> bool {
  if (native.sample_count <= 0 ||
      native.sample_count > static_cast<int32_t>(movement::kMaxMovementCurveSamples) ||
      native.samples == nullptr) {
    return false;
  }

  curve.id = native.curve_id;
  curve.sample_count = static_cast<uint16_t>(native.sample_count);
  for (int32_t i = 0; i < native.sample_count; ++i) {
    curve.samples[static_cast<std::size_t>(i)] = native.samples[i];
  }
  return movement::IsMovementCurveValid(curve);
}

}  // namespace

// Mirrors [UnmanagedCallersOnly] exports in Atlas.Runtime. Append-only;
// SetNativeCallbacks clamps to the caller's len.
#pragma pack(push, 1)
struct CellAppCallbackTable {
  RestoreEntityFn restore_entity;
  GetEntityDataFn get_entity_data;
  EntityDestroyedFn entity_destroyed;
  DispatchRpcFn dispatch_rpc;
  GetOwnerSnapshotFn get_owner_snapshot;
  SerializeEntityFn serialize_entity;
  ProximityEventFn proximity_event;
  CoroOnRpcCompleteFn coro_on_rpc_complete;
  EntityLifecycleCancelFn entity_lifecycle_cancel;
  TimerEventFn timer_event;
  EntityDestroyedFn entity_migrating_out;
  RestoreGhostFn restore_ghost;
  EntityDestroyedFn destroy_ghost;
  TeleportFailedFn teleport_failed;
};
#pragma pack(pop)

CellAppNativeProvider::CellAppNativeProvider(EntityLookupFn lookup) : lookup_(std::move(lookup)) {}

CellAppNativeProvider::CellAppNativeProvider(EntityLookupFn lookup, NetworkInterface& network)
    : lookup_(std::move(lookup)), network_(&network) {}

uint8_t CellAppNativeProvider::GetProcessPrefix() {
  return static_cast<uint8_t>(ProcessType::kCellApp);
}

void CellAppNativeProvider::ReportScriptTick(uint32_t entity_id, uint64_t elapsed_us) {
  if (script_tick_fn_) script_tick_fn_(entity_id, elapsed_us);
}

void CellAppNativeProvider::SendClientRpc(uint32_t entity_id, uint32_t rpc_id, RpcTarget target,
                                          const std::byte* payload, int32_t len,
                                          uint64_t trace_id) {
  if (!IsValidNativePayload(payload, len)) {
    ATLAS_LOG_WARNING("CellApp: SendClientRpc rejected invalid payload len={}", len);
    return;
  }
  if (!IsValidRpcTarget(target)) {
    ATLAS_LOG_WARNING("CellApp: SendClientRpc rejected invalid target={}",
                      static_cast<int>(target));
    return;
  }
  auto* source = lookup_ ? lookup_(entity_id) : nullptr;
  if (!source) {
    ATLAS_LOG_WARNING("CellApp: SendClientRpc: unknown entity_id={}", entity_id);
    return;
  }
  if (!source->IsReal()) {
    ATLAS_LOG_WARNING("CellApp: SendClientRpc on Ghost entity_id={} - rejected", entity_id);
    return;
  }
  if (!network_) {
    ATLAS_LOG_ERROR(
        "CellApp: SendClientRpc: provider was constructed without a NetworkInterface "
        "(handler-level test?); cannot route to BaseApp");
    return;
  }

  // One BroadcastRpcFromCell per destination BaseApp.
  std::unordered_map<Address, std::vector<EntityID>> by_baseapp;
  const EntityID source_entity_id = source->Id();
  const auto& source_base_addr = source->BaseAddr();

  if (target != RpcTarget::kOthers) {
    by_baseapp[source_base_addr].push_back(source_entity_id);
  }
  if (target != RpcTarget::kOwner) {
    // O(W) over observers; independent of population size.
    for (Witness* w : source->Observers()) {
      CellEntity& observer = w->Owner();
      by_baseapp[observer.BaseAddr()].push_back(observer.Id());
    }
  }

  for (auto& [base_addr, ids] : by_baseapp) {
    auto base_ch = network_->ConnectRudpNocwnd(base_addr);
    if (!base_ch) {
      ATLAS_LOG_WARNING("CellApp: SendClientRpc: cannot connect to base at {}",
                        base_addr.ToString());
      continue;
    }
    baseapp::BroadcastRpcFromCell msg;
    msg.rpc_id = rpc_id;
    msg.source_entity_id = source_entity_id;
    msg.dest_entity_ids = std::move(ids);
    msg.trace_id = trace_id;
    if (len > 0) msg.payload.assign(payload, payload + static_cast<std::size_t>(len));
    (void)(*base_ch)->SendMessage(msg);
  }

  // Forward Others/All to each Haunt cell so Observers attached via the
  // remote Ghost mirror also see the RPC. Owner-only stays local.
  if (target == RpcTarget::kOwner) return;
  auto* rd = source->GetRealData();
  if (rd == nullptr) return;
  for (const auto& haunt : rd->Haunts()) {
    if (haunt.channel == nullptr) continue;
    cellapp::ClientRpcBroadcast bmsg;
    bmsg.source_entity_id = source_entity_id;
    bmsg.rpc_id = rpc_id;
    bmsg.target = static_cast<uint8_t>(target);
    bmsg.trace_id = trace_id;
    if (len > 0) bmsg.payload.assign(payload, payload + static_cast<std::size_t>(len));
    (void)haunt.channel->SendMessage(bmsg);
  }
}

void CellAppNativeProvider::SendCellRpc(uint32_t entity_id, uint32_t rpc_id,
                                        const std::byte* payload, int32_t len, uint64_t trace_id) {
  if (!IsValidNativePayload(payload, len)) {
    ATLAS_LOG_WARNING("CellApp: SendCellRpc rejected invalid payload len={}", len);
    return;
  }
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: SendCellRpc: unknown entity_id={}", entity_id);
    return;
  }
  // A Real here means the script could have called the method directly; this
  // path exists only for Ghost->Real cross-cell routing.
  if (entity->IsReal()) {
    ATLAS_LOG_WARNING("CellApp: SendCellRpc on Real entity_id={} rpc_id=0x{:06X} - call directly",
                      entity_id, rpc_id);
    return;
  }
  auto* real_ch = entity->GetRealChannel();
  if (!real_ch) {
    ATLAS_LOG_WARNING("CellApp: SendCellRpc: Ghost entity_id={} has no real_channel", entity_id);
    return;
  }
  cellapp::InternalCellRpc msg;
  msg.target_entity_id = entity_id;
  msg.rpc_id = rpc_id;
  msg.trace_id = trace_id;
  if (len > 0) msg.payload.assign(payload, payload + static_cast<std::size_t>(len));
  if (auto r = real_ch->SendMessage(msg); !r) {
    ATLAS_LOG_DEBUG("CellApp: SendCellRpc send failed (entity={}, rpc_id=0x{:06X}): {}", entity_id,
                    rpc_id, r.Error().Message());
  }
}

auto CellAppNativeProvider::CreateLocalCellEntity(uint16_t type_id, uint32_t space_id, float pos_x,
                                                  float pos_y, float pos_z, float dir_x,
                                                  float dir_y, float dir_z, bool on_ground)
    -> uint32_t {
  if (!create_local_entity_fn_) {
    ATLAS_LOG_ERROR("CellApp: CreateLocalCellEntity: not wired to CellApp (type_id={})", type_id);
    return 0;
  }
  return create_local_entity_fn_(type_id, space_id, pos_x, pos_y, pos_z, dir_x, dir_y, dir_z,
                                 on_ground);
}

void CellAppNativeProvider::DestroyCellEntity(uint32_t entity_id) {
  if (!destroy_local_entity_fn_) {
    ATLAS_LOG_ERROR("CellApp: DestroyCellEntity: not wired to CellApp (entity_id={})", entity_id);
    return;
  }
  destroy_local_entity_fn_(entity_id);
}

auto CellAppNativeProvider::TeleportEntity(uint32_t entity_id, uint32_t target_space_id, float pos_x,
                                           float pos_y, float pos_z, float dir_x, float dir_y,
                                           float dir_z) -> bool {
  if (!teleport_entity_fn_) {
    ATLAS_LOG_ERROR("CellApp: TeleportEntity: not wired to CellApp (entity_id={})", entity_id);
    return false;
  }
  return teleport_entity_fn_(entity_id, target_space_id, pos_x, pos_y, pos_z, dir_x, dir_y, dir_z);
}

void CellAppNativeProvider::SetSpaceData(uint32_t space_id, uint16_t key_id,
                                         const std::byte* value, int32_t len) {
  if (!set_space_data_fn_) {
    ATLAS_LOG_ERROR("CellApp: SetSpaceData: not wired to CellApp (space_id={})", space_id);
    return;
  }
  set_space_data_fn_(space_id, key_id, value, len);
}

void CellAppNativeProvider::RemoveSpaceData(uint32_t space_id, uint16_t key_id) {
  if (!remove_space_data_fn_) {
    ATLAS_LOG_ERROR("CellApp: RemoveSpaceData: not wired to CellApp (space_id={})", space_id);
    return;
  }
  remove_space_data_fn_(space_id, key_id);
}

auto CellAppNativeProvider::LoadCollisionAsset(uint32_t space_id, const char* path,
                                               int32_t len) -> bool {
  if (!IsValidNativeString(path, len) || len == 0) {
    ATLAS_LOG_WARNING("CellApp: LoadCollisionAsset rejected invalid path len={}", len);
    return false;
  }
  if (!load_collision_asset_fn_) {
    ATLAS_LOG_ERROR("CellApp: LoadCollisionAsset: not wired to CellApp (space_id={})",
                    space_id);
    return false;
  }
  return load_collision_asset_fn_(space_id, std::string_view(path, static_cast<size_t>(len)));
}

auto CellAppNativeProvider::LoadNavMesh(uint32_t space_id, const char* collision_path,
                                        int32_t collision_len, const char* params_path,
                                        int32_t params_len) -> bool {
  if (!IsValidNativeString(collision_path, collision_len) || collision_len == 0 ||
      !IsValidNativeString(params_path, params_len) || params_len == 0) {
    ATLAS_LOG_WARNING("CellApp: LoadNavMesh rejected invalid paths len={}/{}", collision_len,
                      params_len);
    return false;
  }
  if (!load_nav_mesh_fn_) {
    ATLAS_LOG_ERROR("CellApp: LoadNavMesh: not wired to CellApp (space_id={})", space_id);
    return false;
  }
  return load_nav_mesh_fn_(
      space_id, std::string_view(collision_path, static_cast<size_t>(collision_len)),
      std::string_view(params_path, static_cast<size_t>(params_len)));
}

auto CellAppNativeProvider::GetEntitySpaceId(uint32_t entity_id) -> uint32_t {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) return 0;
  return entity->GetSpace().Id();
}

void CellAppNativeProvider::SetEntityPosition(uint32_t entity_id, float x, float y, float z) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_set_position: unknown entity_id={}", entity_id);
    return;
  }
  // Ghosts are read-only mirrors; reject (soft guard).
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_set_position on Ghost entity_id={} - rejected", entity_id);
    return;
  }
  entity->SetPosition(math::Vector3{x, y, z});
}

void CellAppNativeProvider::SetEntityDirection(uint32_t entity_id, float x, float y, float z) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_set_direction: unknown entity_id={}", entity_id);
    return;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_set_direction on Ghost entity_id={} - rejected", entity_id);
    return;
  }
  entity->SetDirection(math::Vector3{x, y, z});
}

void CellAppNativeProvider::SetEntityOnGround(uint32_t entity_id, bool on_ground) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_set_on_ground: unknown entity_id={}", entity_id);
    return;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_set_on_ground on Ghost entity_id={} - rejected", entity_id);
    return;
  }
  entity->SetOnGround(on_ground);
}

void CellAppNativeProvider::SetMovementIntent(uint32_t entity_id, float dir_x, float dir_z,
                                              float speed_mps, uint16_t buttons) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_set_movement_intent: unknown entity_id={}", entity_id);
    return;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_set_movement_intent on Ghost entity_id={} - rejected", entity_id);
    return;
  }
  if (!movement_intent_fn_) {
    ATLAS_LOG_ERROR("CellApp: SetMovementIntent: not wired to CellApp (entity_id={})", entity_id);
    return;
  }
  movement_intent_fn_(entity_id, dir_x, dir_z, speed_mps, buttons);
}

auto CellAppNativeProvider::SetMovementCommand(
    uint32_t entity_id, const NativeMovementCommand& native_command) -> bool {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_set_movement_command: unknown entity_id={}", entity_id);
    return false;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_set_movement_command on Ghost entity_id={} - rejected", entity_id);
    return false;
  }
  if (!movement_command_fn_) {
    ATLAS_LOG_ERROR("CellApp: SetMovementCommand: not wired to CellApp (entity_id={})",
                    entity_id);
    return false;
  }

  movement::MovementCommand command;
  if (!ToMovementCommand(native_command, command)) {
    ATLAS_LOG_WARNING("atlas_set_movement_command: invalid enum entity_id={}", entity_id);
    return false;
  }
  return movement_command_fn_(entity_id, command);
}

auto CellAppNativeProvider::ClearMovementCommand(uint32_t entity_id,
                                                 uint32_t command_id) -> bool {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_clear_movement_command: unknown entity_id={}", entity_id);
    return false;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_clear_movement_command on Ghost entity_id={} - rejected",
                      entity_id);
    return false;
  }
  if (!clear_movement_command_fn_) {
    ATLAS_LOG_ERROR("CellApp: ClearMovementCommand: not wired to CellApp (entity_id={})",
                    entity_id);
    return false;
  }
  return clear_movement_command_fn_(entity_id, command_id);
}

auto CellAppNativeProvider::SetMovementCurve(const NativeMovementCurve& native_curve) -> bool {
  if (!movement_curve_fn_) {
    ATLAS_LOG_ERROR("CellApp: SetMovementCurve: not wired to CellApp (curve_id={})",
                    native_curve.curve_id);
    return false;
  }

  movement::MovementCurve curve;
  if (!ToMovementCurve(native_curve, curve)) {
    ATLAS_LOG_WARNING("atlas_set_movement_curve: invalid curve_id={} sample_count={}",
                      native_curve.curve_id, native_curve.sample_count);
    return false;
  }
  return movement_curve_fn_(curve);
}

void CellAppNativeProvider::GetEntityPosition(uint32_t entity_id, float& x, float& y, float& z) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    x = 0;
    y = 0;
    z = 0;
    return;
  }
  const auto& p = entity->Position();
  x = p.x;
  y = p.y;
  z = p.z;
}

void CellAppNativeProvider::GetEntityDirection(uint32_t entity_id, float& x, float& y, float& z) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    x = 0;
    y = 0;
    z = 0;
    return;
  }
  const auto& d = entity->Direction();
  x = d.x;
  y = d.y;
  z = d.z;
}

auto CellAppNativeProvider::GetEntityOnGround(uint32_t entity_id) -> bool {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  return entity != nullptr && entity->OnGround();
}

auto CellAppNativeProvider::TryGetMovementHistorySample(
    uint32_t entity_id, uint32_t server_tick, NativeMovementHistorySample& sample) -> bool {
  sample = {};
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity || !entity->IsReal()) return false;
  if (!movement_history_sample_fn_) return false;
  return movement_history_sample_fn_(entity_id, server_tick, sample);
}

void CellAppNativeProvider::PublishReplicationFrame(
    uint32_t entity_id, bool has_event, bool has_volatile, const std::byte* owner_snap,
    int32_t owner_snap_len, const std::byte* other_snap, int32_t other_snap_len,
    const std::byte* owner_delta, int32_t owner_delta_len, const std::byte* other_delta,
    int32_t other_delta_len) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_publish_replication_frame: unknown entity_id={}", entity_id);
    return;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_publish_replication_frame on Ghost entity_id={} - rejected",
                      entity_id);
    return;
  }

  CellEntity::ReplicationFrame frame;
  if (owner_delta_len > 0 && owner_delta != nullptr) {
    frame.owner_delta.assign(owner_delta, owner_delta + owner_delta_len);
  }
  if (other_delta_len > 0 && other_delta != nullptr) {
    frame.other_delta.assign(other_delta, other_delta + other_delta_len);
  }
  // Adopt current pos/dir (set earlier by SetEntityPosition) so the
  // volatile branch doesn't overwrite with stale zeros.
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

  entity->PublishReplicationFrame(std::move(frame), has_event, has_volatile, owner_snap_span,
                                  other_snap_span);

  // Owner-scope direct path because Witness skips its owner peer.
  // Envelope bytes match Witness output; client uses the same decoder.
  if (has_event && owner_delta_len > 0 && entity->HasWitness() && network_) {
    const auto* state = entity->GetReplicationState();
    if (state == nullptr) return;
    const uint64_t event_seq = state->latest_event_seq;
    auto base_ch = network_->ConnectRudpNocwnd(entity->BaseAddr());
    if (base_ch) {
      const EntityID owner_entity_id = entity->Id();
      std::vector<std::byte> envelope;
      envelope.reserve(1 + 4 + 8 + static_cast<std::size_t>(owner_delta_len));
      envelope.push_back(static_cast<std::byte>(CellAoIEnvelopeKind::kEntityPropertyUpdate));
      for (int i = 0; i < 4; ++i)
        envelope.push_back(static_cast<std::byte>((owner_entity_id >> (i * 8)) & 0xFF));
      for (int i = 0; i < 8; ++i)
        envelope.push_back(static_cast<std::byte>((event_seq >> (i * 8)) & 0xFF));
      envelope.insert(envelope.end(), owner_delta, owner_delta + owner_delta_len);

      baseapp::ReplicatedReliableDeltaFromCell outgoing;
      outgoing.entity_id = owner_entity_id;
      outgoing.delta = std::move(envelope);
      (void)(*base_ch)->SendMessage(outgoing);
    }
  }
}

auto CellAppNativeProvider::AddMoveController(uint32_t entity_id, float dest_x, float dest_y,
                                              float dest_z, float speed, int32_t user_arg)
    -> int32_t {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_add_move_controller: unknown entity_id={}", entity_id);
    return 0;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_add_move_controller on Ghost entity_id={} - rejected", entity_id);
    return 0;
  }
  return static_cast<int32_t>(entity->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{dest_x, dest_y, dest_z}, speed,
                                              /*face_movement=*/false),
      /*motion=*/entity, user_arg));
}

auto CellAppNativeProvider::AddNavMoveController(uint32_t entity_id, float dest_x, float dest_y,
                                                 float dest_z, float speed, int32_t user_arg)
    -> int32_t {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_add_nav_move_controller: unknown entity_id={}", entity_id);
    return 0;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_add_nav_move_controller on Ghost entity_id={} - rejected", entity_id);
    return 0;
  }
  const nav::NavQueryFilter filter;
  auto path = entity->GetSpace().NavQuery().FindPath(
      entity->Position(), math::Vector3{dest_x, dest_y, dest_z}, filter);
  if (path.status == nav::NavPathStatus::kEmpty) {
    // Routine for AI (off-mesh or unreachable goal); the 0 return is the signal.
    ATLAS_LOG_DEBUG(
        "atlas_add_nav_move_controller: no path for entity {} to ({:.2f},{:.2f},{:.2f})",
        entity_id, dest_x, dest_y, dest_z);
    return 0;
  }
  // A partial path still moves the entity to the closest reachable point;
  // scripts observe arrival via position, not via the returned status.
  return static_cast<int32_t>(entity->GetControllers().Add(
      std::make_unique<MoveAlongPathController>(std::move(path.waypoints), speed,
                                                /*face_movement=*/true),
      /*motion=*/entity, user_arg));
}

auto CellAppNativeProvider::AddTimerController(uint32_t entity_id, float interval, bool repeat,
                                               int32_t user_arg) -> int32_t {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_add_timer_controller: unknown entity_id={}", entity_id);
    return 0;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_add_timer_controller on Ghost entity_id={} - rejected", entity_id);
    return 0;
  }
  auto on_fire = [this, entity_id, user_arg](TimerController& /*self*/) {
    if (timer_event_fn_ == nullptr) return;
    timer_event_fn_(entity_id, user_arg);
  };
  return static_cast<int32_t>(entity->GetControllers().Add(
      std::make_unique<TimerController>(interval, repeat, std::move(on_fire)),
      /*motion=*/nullptr, user_arg));
}

auto CellAppNativeProvider::AddProximityController(uint32_t entity_id, float range,
                                                   int32_t user_arg) -> int32_t {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_add_proximity_controller: unknown entity_id={}", entity_id);
    return 0;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_add_proximity_controller on Ghost entity_id={} - rejected", entity_id);
    return 0;
  }
  // Lambdas carry the script handle; RangeListOrder filters non-entity
  // crossings to match AoITrigger::OwnerOf.
  auto dispatch = [this, entity_id, user_arg](RangeListNode& other, uint8_t is_enter) {
    if (proximity_event_fn_ == nullptr) return;
    if (other.Order() != RangeListOrder::kEntity) return;
    auto* peer = static_cast<EntityRangeListNode&>(other).Owner();
    if (peer == nullptr) return;
    proximity_event_fn_(entity_id, user_arg, peer->Id(), is_enter);
  };
  auto on_enter = [dispatch](ProximityController&, RangeListNode& other) {
    dispatch(other, /*is_enter=*/1);
  };
  auto on_leave = [dispatch](ProximityController&, RangeListNode& other) {
    dispatch(other, /*is_enter=*/0);
  };
  return static_cast<int32_t>(entity->GetControllers().Add(
      std::make_unique<ProximityController>(entity->RangeNode(), entity->GetSpace().GetRangeList(),
                                            range, std::move(on_enter), std::move(on_leave)),
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
  // Minimum = original 4 entries (restore + get_data + destroyed + dispatch).
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
  // nullptr => Offload ships empty persistent_blob (replication baseline covers it).
  serialize_entity_fn_ = table.serialize_entity;
  // nullptr => TickClientBaselinePump short-circuits.
  get_owner_snapshot_fn_ = table.get_owner_snapshot;
  // nullptr => trigger still fires for Offload bookkeeping, but script
  // onProximityEnter/Leave never run.
  proximity_event_fn_ = table.proximity_event;
  entity_lifecycle_cancel_fn_ = table.entity_lifecycle_cancel;
  timer_event_fn_ = table.timer_event;
  entity_migrating_out_fn_ = table.entity_migrating_out;
  // nullptr on older runtimes; Ghost stays a C++-only mirror (legacy).
  restore_ghost_fn_ = table.restore_ghost;
  destroy_ghost_fn_ = table.destroy_ghost;
  teleport_failed_fn_ = table.teleport_failed;
  ATLAS_LOG_INFO("CellApp: native callback table registered (len={})", len);
}

}  // namespace atlas
