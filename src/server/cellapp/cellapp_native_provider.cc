#include "cellapp_native_provider.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "cellapp_messages.h"
#include "foundation/log.h"
#include "real_entity_data.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "server/server_config.h"
#include "space.h"
#include "space/entity_range_list_node.h"
#include "space/move_controller.h"
#include "space/proximity_controller.h"
#include "space/timer_controller.h"
#include "witness.h"

namespace atlas {

namespace {

auto IsValidNativePayload(const std::byte* payload, int32_t len) -> bool {
  return len >= 0 && (payload != nullptr || len == 0);
}

auto IsValidRpcTarget(RpcTarget target) -> bool {
  return target == RpcTarget::kOwner || target == RpcTarget::kOthers || target == RpcTarget::kAll;
}

}  // namespace

// Mirrors [UnmanagedCallersOnly] exports in Atlas.Runtime; same layout
// as BaseApp's table. Append-only; SetNativeCallbacks clamps to caller's
// len so missing entries read back as nullptr.
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
};
#pragma pack(pop)

CellAppNativeProvider::CellAppNativeProvider(EntityLookupFn lookup) : lookup_(std::move(lookup)) {}

CellAppNativeProvider::CellAppNativeProvider(EntityLookupFn lookup, NetworkInterface& network)
    : lookup_(std::move(lookup)), network_(&network) {}

uint8_t CellAppNativeProvider::GetProcessPrefix() {
  return static_cast<uint8_t>(ProcessType::kCellApp);
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
    ATLAS_LOG_WARNING("CellApp: SendClientRpc on Ghost entity_id={} — rejected", entity_id);
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
    ATLAS_LOG_WARNING("CellApp: SendCellRpc on Real entity_id={} rpc_id=0x{:06X} — call directly",
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
    ATLAS_LOG_WARNING("atlas_set_position on Ghost entity_id={} — rejected", entity_id);
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
    ATLAS_LOG_WARNING("atlas_set_direction on Ghost entity_id={} — rejected", entity_id);
    return;
  }
  entity->SetDirection(math::Vector3{x, y, z});
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
    ATLAS_LOG_WARNING("atlas_publish_replication_frame on Ghost entity_id={} — rejected",
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

  // Owner-scope direct path: Witness skips `&peer == &owner_`, so its
  // AoI pump never carries owner-visible property changes. Envelope
  // is byte-identical to Witness output; client uses the same decoder.
  // Seq comes from the freshly allocated state.latest_event_seq.
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
    ATLAS_LOG_WARNING("atlas_add_move_controller on Ghost entity_id={} — rejected", entity_id);
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
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_add_timer_controller on Ghost entity_id={} — rejected", entity_id);
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
    ATLAS_LOG_WARNING("atlas_add_proximity_controller on Ghost entity_id={} — rejected", entity_id);
    return 0;
  }
  // Lambdas capture (this, entity_id, user_arg) so each controller's
  // events carry its script handle. RangeListOrder check filters non-
  // entity crossings (matches AoITrigger::OwnerOf).
  auto dispatch = [this, entity_id, user_arg](RangeListNode& other, uint8_t is_enter) {
    if (proximity_event_fn_ == nullptr) return;
    if (other.Order() != RangeListOrder::kEntity) return;
    auto* peer = static_cast<CellEntity*>(static_cast<EntityRangeListNode&>(other).OwnerData());
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
  entity_lifecycle_cancel_fn_ = table.entity_lifecycle_cancel;  // nullptr on older runtimes
  timer_event_fn_ = table.timer_event;  // nullptr on older runtimes; TimerController fire becomes no-op
  entity_migrating_out_fn_ = table.entity_migrating_out;  // nullptr on older runtimes
  // nullptr on older runtimes — Ghost stays a C++-only mirror (legacy).
  restore_ghost_fn_ = table.restore_ghost;
  destroy_ghost_fn_ = table.destroy_ghost;
  ATLAS_LOG_INFO("CellApp: native callback table registered (len={})", len);
}

}  // namespace atlas
