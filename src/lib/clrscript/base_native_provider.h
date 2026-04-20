#ifndef ATLAS_LIB_CLRSCRIPT_BASE_NATIVE_PROVIDER_H_
#define ATLAS_LIB_CLRSCRIPT_BASE_NATIVE_PROVIDER_H_

#include <cstddef>
#include <cstdint>

#include "clrscript/native_api_provider.h"

namespace atlas {

// ============================================================================
// BaseNativeProvider — default INativeApiProvider implementation
// ============================================================================
//
// Provides:
//   • LogMessage  — routes to the Atlas logging system
//   • ServerTime / DeltaTime — returns 0 (override in concrete subclass)
//   • All RPC functions — logs an error and no-ops (not all processes support
//     every RPC direction; concrete subclasses override what they support)
//   • RegisterEntityType / UnregisterAllEntityTypes — forwards to EntityDefRegistry
//
// Concrete process providers (BaseAppNativeProvider, CellAppNativeProvider,
// …) inherit from this class and override only the methods they support.

class BaseNativeProvider : public INativeApiProvider {
 public:
  // ---- Logging --------------------------------------------------------
  void LogMessage(int32_t level, const char* msg, int32_t len) override;

  // ---- Time (stub — override in concrete provider) --------------------
  double ServerTime() override;
  float DeltaTime() override;

  // ---- Process identity (stub) ----------------------------------------
  uint8_t GetProcessPrefix() override;

  // ---- RPC (default: log error + no-op) --------------------------------
  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                     int32_t len) override;

  void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                   int32_t len) override;

  void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                   int32_t len) override;

  // ---- Entity type registry (forwards to EntityDefRegistry singleton) --
  void RegisterEntityType(const std::byte* data, int32_t len) override;
  void UnregisterAllEntityTypes() override;

  // ---- Persistence (default: log error + no-op) -----------------------
  void WriteToDb(uint32_t entity_id, const std::byte* entity_data, int32_t len) override;

  // ---- Client transfer (default: log error + no-op) -------------------
  void GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) override;

  // ---- Script-initiated entity creation (default: log error + 0) -----
  auto CreateBaseEntity(uint16_t type_id) -> uint32_t override;

  // ---- Callback table (default: no-op) --------------------------------
  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

  // ---- CellApp-specific (default: log + no-op) ------------------------
  //
  // Processes that aren't CellApp inherit these no-op bodies — a script
  // running on BaseApp that calls atlas_set_position just logs an error.
  // CellAppNativeProvider overrides each to wire into Space / CellEntity.
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

 protected:
  BaseNativeProvider() = default;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_BASE_NATIVE_PROVIDER_H_
