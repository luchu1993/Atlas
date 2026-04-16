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
  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, uint8_t target, const std::byte* payload,
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

  // ---- Callback table (default: no-op) --------------------------------
  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

 protected:
  BaseNativeProvider() = default;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_BASE_NATIVE_PROVIDER_H_
