#ifndef ATLAS_LIB_CLRSCRIPT_BASE_NATIVE_PROVIDER_H_
#define ATLAS_LIB_CLRSCRIPT_BASE_NATIVE_PROVIDER_H_

#include <cstddef>
#include <cstdint>

#include "clrscript/native_api_provider.h"

namespace atlas {

class BaseNativeProvider : public INativeApiProvider {
 public:
  void LogMessage(int32_t level, const char* msg, int32_t len) override;

  double ServerTime() override;
  float DeltaTime() override;

  uint8_t GetProcessPrefix() override;

  void SendClientRpc(uint32_t entity_id, uint32_t rpc_id, RpcTarget target,
                     const std::byte* payload, int32_t len) override;

  void SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                   int32_t len) override;

  void SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                   int32_t len) override;

  void RegisterEntityType(const std::byte* data, int32_t len) override;
  void UnregisterAllEntityTypes() override;
  void RegisterStruct(const std::byte* data, int32_t len) override;

  void WriteToDb(uint32_t entity_id, const std::byte* entity_data, int32_t len) override;

  void GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) override;

  auto CreateBaseEntity(uint16_t type_id, uint32_t space_id) -> uint32_t override;

  void SetAoIRadius(uint32_t entity_id, float radius, float hysteresis) override;

  void SetNativeCallbacks(const void* native_callbacks, int32_t len) override;

  // Processes that are not CellApp inherit these no-op bodies.
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

  void ReportClientEventSeqGap(uint32_t entity_id, uint32_t gap_delta) override;

 protected:
  BaseNativeProvider() = default;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_BASE_NATIVE_PROVIDER_H_
