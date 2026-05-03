#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_

#include <cstddef>
#include <cstdint>
#include <functional>

#include "baseapp/baseapp_native_provider.h"  // RestoreEntityFn, DispatchRpcFn, etc.
#include "clrscript/base_native_provider.h"

namespace atlas {

class CellEntity;
class NetworkInterface;

// INativeApiProvider for a CellApp process; thin façade - uses a
// caller-supplied lookup so tests share the class without knowing
// CellApp internals.
class CellAppNativeProvider : public BaseNativeProvider {
 public:
  // Returns nullptr for unknown ids; methods log+skip rather than crash.
  using EntityLookupFn = std::function<CellEntity*(uint32_t entity_id)>;

  // `network` only needed for SendClientRpc (handler tests can omit it).
  explicit CellAppNativeProvider(EntityLookupFn lookup);
  CellAppNativeProvider(EntityLookupFn lookup, NetworkInterface& network);

  uint8_t GetProcessPrefix() override;

  // kOwner targets the source's bound client; kOthers/kAll fan out to
  // every witness with source in AoI, grouped by base_addr.
  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, RpcTarget target,
                     const std::byte* payload, int32_t len) override;

  // CellApp-specific surfaces.
  void SetEntityPosition(uint32_t entity_id, float x, float y, float z) override;
  void PublishReplicationFrame(uint32_t entity_id, uint64_t event_seq, uint64_t volatile_seq,
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

 private:
  EntityLookupFn lookup_;
  NetworkInterface* network_{nullptr};  // null in handler-level tests
  RestoreEntityFn restore_entity_fn_{nullptr};
  DispatchRpcFn dispatch_rpc_fn_{nullptr};
  EntityDestroyedFn entity_destroyed_fn_{nullptr};
  // nullptr until C# registers the expanded NativeCallbackTable;
  // absence: Offload ships empty persistent_blob (replication baseline
  // covers it).
  SerializeEntityFn serialize_entity_fn_{nullptr};
  // nullptr => baseline pump short-circuits.
  GetOwnerSnapshotFn get_owner_snapshot_fn_{nullptr};
  // nullptr => proximity events dropped at lambda; trigger state still
  // correct for Offload / InsidePeers.
  ProximityEventFn proximity_event_fn_{nullptr};
  // nullptr on older runtimes — offload still proceeds, in-flight RPCs
  // fall back to their timeouts.
  EntityLifecycleCancelFn entity_lifecycle_cancel_fn_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_NATIVE_PROVIDER_H_
