#include "cellapp_native_provider.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "foundation/log.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
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
//
// Growth rule: new entries go at the tail. Older C# runtimes produce a
// shorter table; SetNativeCallbacks below clamps the copy to what the
// caller actually sent, so missing entries read back as nullptr.
#pragma pack(push, 1)
struct CellAppCallbackTable {
  RestoreEntityFn restore_entity;
  GetEntityDataFn get_entity_data;
  EntityDestroyedFn entity_destroyed;
  DispatchRpcFn dispatch_rpc;
  GetOwnerSnapshotFn get_owner_snapshot;
  SerializeEntityFn serialize_entity;  // Phase 11 PR-6
};
#pragma pack(pop)

CellAppNativeProvider::CellAppNativeProvider(EntityLookupFn lookup) : lookup_(std::move(lookup)) {}

CellAppNativeProvider::CellAppNativeProvider(EntityLookupFn lookup, NetworkInterface& network)
    : lookup_(std::move(lookup)), network_(&network) {}

uint8_t CellAppNativeProvider::GetProcessPrefix() {
  return static_cast<uint8_t>(ProcessType::kCellApp);
}

void CellAppNativeProvider::SendClientRpc(uint32_t entity_id, uint32_t rpc_id,
                                          const std::byte* payload, int32_t len) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: SendClientRpc: unknown entity_id={}", entity_id);
    return;
  }
  if (!network_) {
    ATLAS_LOG_ERROR(
        "CellApp: SendClientRpc: provider was constructed without a NetworkInterface "
        "(handler-level test?); cannot route to BaseApp");
    return;
  }
  // Route back to the owning BaseApp via SelfRpcFromCell. BaseApp's
  // OnSelfRpcFromCell relays to the client on the RUDP channel using
  // rpc_id as the wire msg_id.
  auto base_ch = network_->ConnectRudpNocwnd(entity->BaseAddr());
  if (!base_ch) {
    ATLAS_LOG_ERROR("CellApp: SendClientRpc: cannot connect to base at {} for entity {}",
                    entity->BaseAddr().ToString(), entity_id);
    return;
  }
  baseapp::SelfRpcFromCell msg;
  msg.base_entity_id = entity->BaseEntityId();
  msg.rpc_id = rpc_id;
  msg.payload.assign(payload, payload + len);
  (void)(*base_ch)->SendMessage(msg);
}

void CellAppNativeProvider::SetEntityPosition(uint32_t entity_id, float x, float y, float z) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) {
    ATLAS_LOG_WARNING("atlas_set_position: unknown entity_id={}", entity_id);
    return;
  }
  // Phase 11 §3.1: Ghosts are read-only mirrors of a remote Real. A script
  // running on this CellApp that tries to write a Ghost's position is
  // almost always a logic bug — log and drop (Q2: soft guard).
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_set_position on Ghost entity_id={} — rejected", entity_id);
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
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_publish_replication_frame on Ghost entity_id={} — rejected",
                      entity_id);
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

  // Owner-scope direct forward. Witness::HandleAoIEnter skips `&peer ==
  // &owner_`, so its AoI pump never carries the owner's own client-visible
  // property changes — observer_is_owner in Witness::SendEntityUpdate is
  // always false and the generator-emitted owner_delta would otherwise
  // die here. BigWorld solves this in the opposite direction (push per
  // change, bigworld server/cellapp/entity.cpp:5670), but the effect is
  // the same: own-scope flows on a dedicated path that does not go
  // through the AoI witness.
  //
  // Routing: envelope [kind=kEntityPropertyUpdate][base_id u32][event_seq
  // u64][owner_delta bytes] → ReplicatedReliableDeltaFromCell (msg 2017) to
  // the BaseApp → 0xF003 to the client. Format is byte-identical to what
  // Witness produces for peer deltas, so Atlas.Client.ClientCallbacks
  // decodes both paths with the same DispatchAoIEnvelope handler.
  if (owner_delta_len > 0 && event_seq > 0 && entity->HasWitness() && network_) {
    auto base_ch = network_->ConnectRudpNocwnd(entity->BaseAddr());
    if (base_ch) {
      const auto base_id = entity->BaseEntityId();
      std::vector<std::byte> envelope;
      envelope.reserve(1 + 4 + 8 + static_cast<std::size_t>(owner_delta_len));
      envelope.push_back(static_cast<std::byte>(CellAoIEnvelopeKind::kEntityPropertyUpdate));
      for (int i = 0; i < 4; ++i)
        envelope.push_back(static_cast<std::byte>((base_id >> (i * 8)) & 0xFF));
      for (int i = 0; i < 8; ++i)
        envelope.push_back(static_cast<std::byte>((event_seq >> (i * 8)) & 0xFF));
      envelope.insert(envelope.end(), owner_delta, owner_delta + owner_delta_len);

      baseapp::ReplicatedReliableDeltaFromCell outgoing;
      outgoing.base_entity_id = base_id;
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
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_add_proximity_controller on Ghost entity_id={} — rejected", entity_id);
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
  // serialize_entity was appended in PR-6. Older runtimes leave it as
  // the memset-initialised nullptr; CellApp treats that as "no C# blob
  // available" and continues serving Offload with just the replication
  // baseline.
  serialize_entity_fn_ = table.serialize_entity;
  // L4: get_owner_snapshot feeds CellApp::TickClientBaselinePump; without
  // it the pump short-circuits and no baseline leaves the cell (acceptable
  // on tests without a C# runtime). get_entity_data is CellApp-side
  // unused today (no DB persistence from cell).
  get_owner_snapshot_fn_ = table.get_owner_snapshot;
  ATLAS_LOG_INFO("CellApp: native callback table registered (len={})", len);
}

}  // namespace atlas
