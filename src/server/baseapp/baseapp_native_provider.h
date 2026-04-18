#ifndef ATLAS_SERVER_BASEAPP_BASEAPP_NATIVE_PROVIDER_H_
#define ATLAS_SERVER_BASEAPP_BASEAPP_NATIVE_PROVIDER_H_

#include <cstddef>
#include <cstdint>
#include <functional>

#include "clrscript/base_native_provider.h"
#include "server/entity_types.h"

namespace atlas {

class BaseApp;

// ============================================================================
// RestoreEntityFn — function pointer type for C++ → C# entity restoration
//
// Called by BaseApp after loading entity data from DB, so that C# Atlas.Runtime
// can hydrate the entity object from the serialised property blob.
//
// Parameters:
//   entity_id — the live EntityID assigned by EntityManager
//   type_id   — entity type
//   dbid      — DatabaseID (0 if not yet persisted)
//   data      — serialised property blob
//   len       — byte length of data
// ============================================================================

using RestoreEntityFn = void (*)(uint32_t entity_id, uint16_t type_id, int64_t dbid,
                                 const uint8_t* data, int32_t len);

// ============================================================================
// GetEntityDataFn — called by BaseApp before write_to_db to retrieve the
// current serialised property blob from C#.
// ============================================================================

using GetEntityDataFn = void (*)(uint32_t entity_id, uint8_t** out_data, int32_t* out_len);

// ============================================================================
// EntityDestroyedFn — notifies C# that an entity has been removed from the
// native EntityManager so that the managed object can be collected.
// ============================================================================

using EntityDestroyedFn = void (*)(uint32_t entity_id);

// ============================================================================
// DispatchRpcFn — dispatches an incoming RPC to the C# entity.
//
// Called by C++ when a validated RPC arrives (e.g. ClientBaseRpc from external
// interface or CellRpcForward from internal interface).
//
// Parameters:
//   entity_id — target entity
//   rpc_id    — packed RPC ID [direction:2 | typeIndex:14 | method:8]
//   payload   — serialised argument data
//   len       — byte length of payload
// ============================================================================

using DispatchRpcFn = void (*)(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload,
                               int32_t len);

// ============================================================================
// GetOwnerSnapshotFn — called by BaseApp's periodic baseline pump to fetch the
// owner-scope serialization of an entity (the fields visible to the owning
// client). Differs from GetEntityData (which is scope-agnostic full-entity DB
// state) by filtering to client-visible properties via the source-generated
// SerializeForOwnerClient method. Callee fills a pinned buffer and returns the
// pointer + length; out_len is -1 on error.
// ============================================================================

using GetOwnerSnapshotFn = void (*)(uint32_t entity_id, uint8_t** out_data, int32_t* out_len);

// ============================================================================
// BaseAppNativeProvider — INativeApiProvider for the BaseApp process
// ============================================================================

class BaseAppNativeProvider : public BaseNativeProvider {
 public:
  explicit BaseAppNativeProvider(BaseApp& app);

  // ---- Process identity -----------------------------------------------
  uint8_t GetProcessPrefix() override;

  // ---- RPC dispatch ---------------------------------------------------
  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, uint8_t target, const std::byte* payload,
                     int32_t len) override;

  void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                   int32_t len) override;

  // SendBaseRpc: forward to another BaseEntity on this BaseApp
  void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                   int32_t len) override;

  // ---- Persistence ----------------------------------------------------
  void WriteToDb(uint32_t entity_id, const std::byte* entity_data, int32_t len) override;

  // ---- Client transfer ------------------------------------------------
  void GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) override;

  // ---- C# → C++ callback table ----------------------------------------
  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

  // ---- Callback accessors (used by BaseApp message handlers) ----------
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
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_BASEAPP_NATIVE_PROVIDER_H_
