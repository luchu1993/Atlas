#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include "baseapp/baseapp_native_provider.h"  // RestoreEntityFn, DispatchRpcFn, etc.
#include "clrscript/base_native_provider.h"
#include "movement_sim/movement_sim.h"

namespace atlas {

class CellEntity;
class NetworkInterface;

// INativeApiProvider for CellApp; a lookup function keeps tests decoupled
// from CellApp internals.
class CellAppNativeProvider : public BaseNativeProvider {
 public:
  // Returns nullptr for unknown ids; methods log+skip rather than crash.
  using EntityLookupFn = std::function<CellEntity*(uint32_t entity_id)>;
  using CreateLocalEntityFn =
      std::function<uint32_t(uint16_t type_id, uint32_t space_id, float pos_x, float pos_y,
                             float pos_z, float dir_x, float dir_y, float dir_z, bool on_ground)>;
  using DestroyLocalEntityFn = std::function<void(uint32_t entity_id)>;
  using TeleportEntityFn =
      std::function<bool(uint32_t entity_id, uint32_t target_space_id, float pos_x, float pos_y,
                         float pos_z, float dir_x, float dir_y, float dir_z)>;
  using SetSpaceDataFn = std::function<void(uint32_t space_id, uint16_t key_id,
                                            const std::byte* value, int32_t len)>;
  using RemoveSpaceDataFn = std::function<void(uint32_t space_id, uint16_t key_id)>;
  using LoadCollisionAssetFn = std::function<bool(uint32_t space_id, std::string_view path)>;
  using LoadNavMeshFn = std::function<bool(uint32_t space_id, std::string_view collision_path,
                                           std::string_view params_path)>;
  using ScriptTickFn = std::function<void(uint32_t entity_id, uint64_t elapsed_us)>;
  using MovementIntentFn = std::function<void(uint32_t entity_id, float dir_x, float dir_z,
                                              float speed_mps, uint16_t buttons)>;
  using MovementCommandFn =
      std::function<bool(uint32_t entity_id, const movement::MovementCommand& command)>;
  using ClearMovementCommandFn =
      std::function<bool(uint32_t entity_id, uint32_t command_id)>;
  using MovementCurveFn = std::function<bool(const movement::MovementCurve& curve)>;
  using MovementHistorySampleFn =
      std::function<bool(uint32_t entity_id, uint32_t server_tick,
                         NativeMovementHistorySample& sample)>;

  // `network` only needed for SendClientRpc (handler tests can omit it).
  explicit CellAppNativeProvider(EntityLookupFn lookup);
  CellAppNativeProvider(EntityLookupFn lookup, NetworkInterface& network);

  // CellApp wires these at startup; tests may inject mocks.
  void SetCreateLocalEntityFn(CreateLocalEntityFn fn) { create_local_entity_fn_ = std::move(fn); }
  void SetDestroyLocalEntityFn(DestroyLocalEntityFn fn) {
    destroy_local_entity_fn_ = std::move(fn);
  }
  void SetTeleportEntityFn(TeleportEntityFn fn) { teleport_entity_fn_ = std::move(fn); }
  void SetSetSpaceDataFn(SetSpaceDataFn fn) { set_space_data_fn_ = std::move(fn); }
  void SetRemoveSpaceDataFn(RemoveSpaceDataFn fn) { remove_space_data_fn_ = std::move(fn); }
  void SetLoadCollisionAssetFn(LoadCollisionAssetFn fn) {
    load_collision_asset_fn_ = std::move(fn);
  }
  void SetLoadNavMeshFn(LoadNavMeshFn fn) { load_nav_mesh_fn_ = std::move(fn); }
  void SetScriptTickFn(ScriptTickFn fn) { script_tick_fn_ = std::move(fn); }
  void SetMovementIntentFn(MovementIntentFn fn) { movement_intent_fn_ = std::move(fn); }
  void SetMovementCommandFn(MovementCommandFn fn) {
    movement_command_fn_ = std::move(fn);
  }
  void SetClearMovementCommandFn(ClearMovementCommandFn fn) {
    clear_movement_command_fn_ = std::move(fn);
  }
  void SetMovementCurveFn(MovementCurveFn fn) { movement_curve_fn_ = std::move(fn); }
  void SetMovementHistorySampleFn(MovementHistorySampleFn fn) {
    movement_history_sample_fn_ = std::move(fn);
  }

  uint8_t GetProcessPrefix() override;
  void ReportScriptTick(uint32_t entity_id, uint64_t elapsed_us) override;

  // kOwner targets the source's bound client; kOthers/kAll fan out to
  // every witness with source in AoI, grouped by base_addr.
  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, RpcTarget target,
                     const std::byte* payload, int32_t len, uint64_t trace_id) override;

  // Ghost-side script invokes a cell method on its Real owner. Calls on a
  // local Real warn because scripts should invoke directly in-process.
  void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload, int32_t len,
                   uint64_t trace_id) override;

  auto CreateLocalCellEntity(uint16_t type_id, uint32_t space_id, float pos_x, float pos_y,
                             float pos_z, float dir_x, float dir_y, float dir_z, bool on_ground)
      -> uint32_t override;
  void DestroyCellEntity(uint32_t entity_id) override;
  auto TeleportEntity(uint32_t entity_id, uint32_t target_space_id, float pos_x, float pos_y,
                      float pos_z, float dir_x, float dir_y, float dir_z) -> bool override;

  void SetSpaceData(uint32_t space_id, uint16_t key_id, const std::byte* value,
                    int32_t len) override;
  void RemoveSpaceData(uint32_t space_id, uint16_t key_id) override;
  auto LoadCollisionAsset(uint32_t space_id, const char* path, int32_t len) -> bool override;
  auto LoadNavMesh(uint32_t space_id, const char* collision_path, int32_t collision_len,
                   const char* params_path, int32_t params_len) -> bool override;

  auto GetEntitySpaceId(uint32_t entity_id) -> uint32_t override;

  // CellApp-specific surfaces.
  void SetEntityPosition(uint32_t entity_id, float x, float y, float z) override;
  void SetEntityDirection(uint32_t entity_id, float x, float y, float z) override;
  void SetEntityOnGround(uint32_t entity_id, bool on_ground) override;
  void SetMovementIntent(uint32_t entity_id, float dir_x, float dir_z, float speed_mps,
                         uint16_t buttons) override;
  auto SetMovementCommand(uint32_t entity_id, const NativeMovementCommand& command)
      -> bool override;
  auto ClearMovementCommand(uint32_t entity_id, uint32_t command_id) -> bool override;
  auto SetMovementCurve(const NativeMovementCurve& curve) -> bool override;
  void GetEntityPosition(uint32_t entity_id, float& x, float& y, float& z) override;
  void GetEntityDirection(uint32_t entity_id, float& x, float& y, float& z) override;
  auto GetEntityOnGround(uint32_t entity_id) -> bool override;
  auto TryGetMovementHistorySample(uint32_t entity_id, uint32_t server_tick,
                                   NativeMovementHistorySample& sample) -> bool override;
  void PublishReplicationFrame(uint32_t entity_id, bool has_event, bool has_volatile,
                               const std::byte* owner_snap, int32_t owner_snap_len,
                               const std::byte* other_snap, int32_t other_snap_len,
                               const std::byte* owner_delta, int32_t owner_delta_len,
                               const std::byte* other_delta, int32_t other_delta_len) override;
  auto AddMoveController(uint32_t entity_id, float dest_x, float dest_y, float dest_z, float speed,
                         int32_t user_arg) -> int32_t override;
  auto AddTimerController(uint32_t entity_id, float interval, bool repeat, int32_t user_arg)
      -> int32_t override;
  auto AddProximityController(uint32_t entity_id, float range, int32_t user_arg)
      -> int32_t override;
  void CancelController(uint32_t entity_id, int32_t controller_id) override;

  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

  [[nodiscard]] auto restore_entity_fn() const -> RestoreEntityFn { return restore_entity_fn_; }
  [[nodiscard]] auto dispatch_rpc_fn() const -> DispatchRpcFn { return dispatch_rpc_fn_; }
  [[nodiscard]] auto entity_destroyed_fn() const -> EntityDestroyedFn {
    return entity_destroyed_fn_;
  }
  [[nodiscard]] auto serialize_entity_fn() const -> SerializeEntityFn {
    return serialize_entity_fn_;
  }
  // Owner-scope serializer drives CellApp::TickClientBaselinePump.
  [[nodiscard]] auto get_owner_snapshot_fn() const -> GetOwnerSnapshotFn {
    return get_owner_snapshot_fn_;
  }
  // Tests can swap in a recording fn without going through SetNativeCallbacks.
  [[nodiscard]] auto proximity_event_fn() const -> ProximityEventFn { return proximity_event_fn_; }
  void SetProximityEventFnForTest(ProximityEventFn fn) { proximity_event_fn_ = fn; }
  [[nodiscard]] auto entity_lifecycle_cancel_fn() const -> EntityLifecycleCancelFn {
    return entity_lifecycle_cancel_fn_;
  }
  // Silent removal for cross-cellapp migration; drops the C# instance without
  // firing OnDestroy so script-side counters survive the offload.
  [[nodiscard]] auto entity_migrating_out_fn() const -> EntityDestroyedFn {
    return entity_migrating_out_fn_;
  }
  [[nodiscard]] auto restore_ghost_fn() const -> RestoreGhostFn { return restore_ghost_fn_; }
  [[nodiscard]] auto destroy_ghost_fn() const -> EntityDestroyedFn { return destroy_ghost_fn_; }
  // nullptr on older runtimes; teleport failures then go unreported to script.
  [[nodiscard]] auto teleport_failed_fn() const -> TeleportFailedFn { return teleport_failed_fn_; }
  void SetTeleportFailedFnForTest(TeleportFailedFn fn) { teleport_failed_fn_ = fn; }

 private:
  EntityLookupFn lookup_;
  CreateLocalEntityFn create_local_entity_fn_;
  DestroyLocalEntityFn destroy_local_entity_fn_;
  TeleportEntityFn teleport_entity_fn_;
  SetSpaceDataFn set_space_data_fn_;
  RemoveSpaceDataFn remove_space_data_fn_;
  LoadCollisionAssetFn load_collision_asset_fn_;
  LoadNavMeshFn load_nav_mesh_fn_;
  ScriptTickFn script_tick_fn_;
  MovementIntentFn movement_intent_fn_;
  MovementCommandFn movement_command_fn_;
  ClearMovementCommandFn clear_movement_command_fn_;
  MovementCurveFn movement_curve_fn_;
  MovementHistorySampleFn movement_history_sample_fn_;
  NetworkInterface* network_{nullptr};  // null in handler-level tests
  RestoreEntityFn restore_entity_fn_{nullptr};
  DispatchRpcFn dispatch_rpc_fn_{nullptr};
  EntityDestroyedFn entity_destroyed_fn_{nullptr};
  // nullptr until C# registers the expanded NativeCallbackTable; absence
  // means Offload ships empty persistent_blob.
  SerializeEntityFn serialize_entity_fn_{nullptr};
  // nullptr => baseline pump short-circuits.
  GetOwnerSnapshotFn get_owner_snapshot_fn_{nullptr};
  // nullptr => proximity events dropped at lambda; trigger state still
  // correct for Offload / InsidePeers.
  ProximityEventFn proximity_event_fn_{nullptr};
  TimerEventFn timer_event_fn_{nullptr};
  // nullptr on older runtimes; offload still proceeds, in-flight RPCs
  // fall back to their timeouts.
  EntityLifecycleCancelFn entity_lifecycle_cancel_fn_{nullptr};
  // nullptr on older runtimes; offload falls back to entity_destroyed_fn,
  // which is wrong (OnDestroy fires on migration) but keeps things alive.
  EntityDestroyedFn entity_migrating_out_fn_{nullptr};
  // nullptr on older runtimes; Ghost stays C++-only (legacy behaviour).
  RestoreGhostFn restore_ghost_fn_{nullptr};
  EntityDestroyedFn destroy_ghost_fn_{nullptr};
  TeleportFailedFn teleport_failed_fn_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_
