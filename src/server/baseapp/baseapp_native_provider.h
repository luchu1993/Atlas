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

// Dispatches a validated incoming RPC to the C# entity.
// rpc_id is packed [direction:2 | typeIndex:14 | method:8].
using DispatchRpcFn = void (*)(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload,
                               int32_t len);

// Owner-scope baseline snapshot via SerializeForOwnerClient (filtered to
// client-visible fields). Distinct from GetEntityData (scope-agnostic DB
// state). out_len is -1 on error.
using GetOwnerSnapshotFn = void (*)(uint32_t entity_id, uint8_t** out_data, int32_t* out_len);

// Offload serialization. Returns 0 on success, required size if out_buf
// too small, -1 on error (out_len also set to -1).
using SerializeEntityFn = int32_t (*)(uint32_t entity_id, uint8_t* out_buf, int32_t out_buf_cap,
                                      int32_t* out_len);

// Proximity sensor enter/leave callback. user_arg is the script-supplied
// handle from AddProximityController (lets one entity own multiple sensors);
// native side does not interpret it.
using ProximityEventFn = void (*)(uint32_t entity_id, int32_t user_arg, uint32_t peer_entity_id,
                                  uint8_t is_enter);

// INativeApiProvider for the BaseApp process.
class BaseAppNativeProvider : public BaseNativeProvider {
 public:
  explicit BaseAppNativeProvider(BaseApp& app);

  uint8_t GetProcessPrefix() override;

  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, RpcTarget target,
                     const std::byte* payload, int32_t len) override;

  void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                   int32_t len) override;

  // Forward to another BaseEntity on this BaseApp.
  void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                   int32_t len) override;

  void WriteToDb(uint32_t entity_id, const std::byte* entity_data, int32_t len) override;

  void GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) override;

  auto CreateBaseEntity(uint16_t type_id, uint32_t space_id) -> uint32_t override;

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

 private:
  BaseApp& app_;
  RestoreEntityFn restore_entity_fn_{nullptr};
  GetEntityDataFn get_entity_data_fn_{nullptr};
  EntityDestroyedFn entity_destroyed_fn_{nullptr};
  DispatchRpcFn dispatch_rpc_fn_{nullptr};
  GetOwnerSnapshotFn get_owner_snapshot_fn_{nullptr};
  CoroOnRpcCompleteFn coro_on_rpc_complete_fn_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_BASEAPP_NATIVE_PROVIDER_H_
