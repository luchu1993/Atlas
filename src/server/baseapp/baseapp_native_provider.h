#ifndef ATLAS_SERVER_BASEAPP_BASEAPP_NATIVE_PROVIDER_H_
#define ATLAS_SERVER_BASEAPP_BASEAPP_NATIVE_PROVIDER_H_

#include <cstddef>
#include <cstdint>
#include <functional>

#include "clrscript/base_native_provider.h"
#include "clrscript/coro_bridge.h"
#include "server/entity_types.h"

namespace atlas {

class BaseApp;

// C# entity hydration after DB load. dbid=0 when not yet persisted.
using RestoreEntityFn = void (*)(uint32_t entity_id, uint16_t type_id, int64_t dbid,
                                 const uint8_t* data, int32_t len);

// Pre-write_to_db: pull current serialized property blob from C#.
using GetEntityDataFn = void (*)(uint32_t entity_id, uint8_t** out_data, int32_t* out_len);

// Notifies C# that an entity has been removed natively.
using EntityDestroyedFn = void (*)(uint32_t entity_id);

// reply_channel is Channel* recast — C# hands it back via SendEntityRpcReply.
using DispatchRpcFn = void (*)(uint32_t entity_id, uint32_t rpc_id, intptr_t reply_channel,
                               const uint8_t* payload, int32_t len, uint64_t trace_id);

// Owner-scope baseline (SerializeForOwnerClient); out_len is -1 on error.
using GetOwnerSnapshotFn = void (*)(uint32_t entity_id, uint8_t** out_data, int32_t* out_len);

// 0 on success, required size if out_buf too small, -1 on error.
using SerializeEntityFn = int32_t (*)(uint32_t entity_id, uint8_t* out_buf, int32_t out_buf_cap,
                                      int32_t* out_len);

// user_arg is script-supplied (lets one entity own multiple sensors).
using ProximityEventFn = void (*)(uint32_t entity_id, int32_t user_arg, uint32_t peer_entity_id,
                                  uint8_t is_enter);

// Per-entity timer fire callback. user_arg threads the script-supplied id
// back so multiple timers on one entity can disambiguate.
using TimerEventFn = void (*)(uint32_t entity_id, int32_t user_arg);

// Triggers the C# entity's LifecycleCancellation. Fired by CellApp before
// offload and by any process that needs to drain in-flight RPCs early.
using EntityLifecycleCancelFn = void (*)(uint32_t entity_id);

// INativeApiProvider for the BaseApp process.
class BaseAppNativeProvider : public BaseNativeProvider {
 public:
  explicit BaseAppNativeProvider(BaseApp& app);

  uint8_t GetProcessPrefix() override;

  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, RpcTarget target,
                     const std::byte* payload, int32_t len, uint64_t trace_id) override;

  void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload, int32_t len,
                   uint64_t trace_id) override;

  // Forward to another BaseEntity on this BaseApp.
  void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload, int32_t len,
                   uint64_t trace_id) override;

  void WriteToDb(uint32_t entity_id, const std::byte* entity_data, int32_t len) override;

  void GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) override;

  auto CreateBaseEntity(uint16_t type_id, uint32_t space_id) -> uint32_t override;

  auto RequestSpawnCellOnly(uint16_t type_id, uint32_t space_id, float pos_x, float pos_y,
                            float pos_z, float dir_x, float dir_y, float dir_z, bool on_ground)
      -> bool override;

  void SetAoIRadius(uint32_t entity_id, float radius, float hysteresis) override;

  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

  auto CoroRegisterPending(uint16_t reply_id, uint32_t request_id, int32_t timeout_ms,
                           intptr_t managed_handle) -> uint64_t override;
  void CoroCancelPending(uint64_t handle) override;

  [[nodiscard]] auto restore_entity_fn() const -> RestoreEntityFn { return restore_entity_fn_; }
  [[nodiscard]] auto get_entity_data_fn() const -> GetEntityDataFn { return get_entity_data_fn_; }
  [[nodiscard]] auto entity_destroyed_fn() const -> EntityDestroyedFn {
    return entity_destroyed_fn_;
  }
  [[nodiscard]] auto dispatch_rpc_fn() const -> DispatchRpcFn { return dispatch_rpc_fn_; }
  [[nodiscard]] auto get_owner_snapshot_fn() const -> GetOwnerSnapshotFn {
    return get_owner_snapshot_fn_;
  }
  [[nodiscard]] auto entity_lifecycle_cancel_fn() const -> EntityLifecycleCancelFn {
    return entity_lifecycle_cancel_fn_;
  }

 private:
  BaseApp& app_;
  RestoreEntityFn restore_entity_fn_{nullptr};
  GetEntityDataFn get_entity_data_fn_{nullptr};
  EntityDestroyedFn entity_destroyed_fn_{nullptr};
  DispatchRpcFn dispatch_rpc_fn_{nullptr};
  GetOwnerSnapshotFn get_owner_snapshot_fn_{nullptr};
  EntityLifecycleCancelFn entity_lifecycle_cancel_fn_{nullptr};
  CoroOnRpcCompleteFn coro_on_rpc_complete_fn_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_BASEAPP_NATIVE_PROVIDER_H_
