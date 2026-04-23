#include "clrscript/base_native_provider.h"

#include <cstddef>
#include <string_view>

#include "entitydef/entity_def_registry.h"
#include "foundation/log.h"

namespace atlas {

void BaseNativeProvider::LogMessage(int32_t level, const char* msg, int32_t len) {
  // Guard against bad inputs from across the C#/C++ boundary.
  if (msg == nullptr || len <= 0) return;

  std::string_view message(msg, static_cast<std::size_t>(len));
  auto log_level = static_cast<LogLevel>(level);
  // Call Logger directly rather than expanding 6 ATLAS_LOG_* macros.
  // Each macro captures source_location and does compile-time level checks
  // that are meaningless for C#-originated messages.  Direct Logger::Log()
  // is one call with a single runtime level check, and adds a "clr" category
  // to distinguish managed-side log output in filtered views.
  auto& logger = Logger::Instance();
  if (static_cast<uint8_t>(log_level) >= static_cast<uint8_t>(logger.Level()))
    logger.Log(log_level, "clr", message);
}

double BaseNativeProvider::ServerTime() {
  return 0.0;
}

float BaseNativeProvider::DeltaTime() {
  return 0.0f;
}

uint8_t BaseNativeProvider::GetProcessPrefix() {
  ATLAS_LOG_ERROR("get_process_prefix() not implemented for this process type");
  return 0;
}

void BaseNativeProvider::SendClientRpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                       const std::byte* /*payload*/, int32_t /*len*/) {
  ATLAS_LOG_ERROR(
      "send_client_rpc() not supported on this process type "
      "(entity_id={})",
      entity_id);
}

void BaseNativeProvider::SendCellRpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                     const std::byte* /*payload*/, int32_t /*len*/) {
  ATLAS_LOG_ERROR(
      "send_cell_rpc() not supported on this process type "
      "(entity_id={})",
      entity_id);
}

void BaseNativeProvider::SendBaseRpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                     const std::byte* /*payload*/, int32_t /*len*/) {
  ATLAS_LOG_ERROR(
      "send_base_rpc() not supported on this process type "
      "(entity_id={})",
      entity_id);
}

void BaseNativeProvider::RegisterEntityType(const std::byte* data, int32_t len) {
  EntityDefRegistry::Instance().RegisterType(data, len);
}

void BaseNativeProvider::UnregisterAllEntityTypes() {
  EntityDefRegistry::Instance().clear();
}

void BaseNativeProvider::WriteToDb(uint32_t entity_id, const std::byte* /*entity_data*/,
                                   int32_t /*len*/) {
  ATLAS_LOG_ERROR("write_to_db() not supported on this process type (entity_id={})", entity_id);
}

void BaseNativeProvider::GiveClientTo(uint32_t src_entity_id, uint32_t /*dest_entity_id*/) {
  ATLAS_LOG_ERROR("give_client_to() not supported on this process type (src={})", src_entity_id);
}

auto BaseNativeProvider::CreateBaseEntity(uint16_t type_id, uint32_t /*space_id*/) -> uint32_t {
  ATLAS_LOG_ERROR("create_base_entity() not supported on this process type (type_id={})", type_id);
  return 0;
}

void BaseNativeProvider::SetAoIRadius(uint32_t entity_id, float /*radius*/, float /*hysteresis*/) {
  ATLAS_LOG_ERROR("set_aoi_radius() not supported on this process type (entity_id={})", entity_id);
}

void BaseNativeProvider::SetNativeCallbacks(const void* /*native_callbacks*/, int32_t /*len*/) {
  // Default: silently ignore.  Processes without C# scripting never receive callbacks.
}

// ---------------------------------------------------------------------------
// CellApp-specific defaults — every non-CellApp process gets these as
// error-logging no-ops. Game scripts calling atlas_set_position on
// BaseApp (where no RangeList exists) get a clear log instead of a
// silent mis-operation.
// ---------------------------------------------------------------------------

void BaseNativeProvider::SetEntityPosition(uint32_t entity_id, float /*x*/, float /*y*/,
                                           float /*z*/) {
  ATLAS_LOG_ERROR("atlas_set_position() not supported on this process type (entity_id={})",
                  entity_id);
}

void BaseNativeProvider::PublishReplicationFrame(
    uint32_t entity_id, uint64_t /*event_seq*/, uint64_t /*volatile_seq*/,
    const std::byte* /*owner_snap*/, int32_t /*owner_snap_len*/, const std::byte* /*other_snap*/,
    int32_t /*other_snap_len*/, const std::byte* /*owner_delta*/, int32_t /*owner_delta_len*/,
    const std::byte* /*other_delta*/, int32_t /*other_delta_len*/) {
  ATLAS_LOG_ERROR(
      "atlas_publish_replication_frame() not supported on this process type (entity_id={})",
      entity_id);
}

auto BaseNativeProvider::AddMoveController(uint32_t entity_id, float /*dx*/, float /*dy*/,
                                           float /*dz*/, float /*speed*/, int32_t /*user_arg*/)
    -> int32_t {
  ATLAS_LOG_ERROR("atlas_add_move_controller() not supported on this process type (entity_id={})",
                  entity_id);
  return 0;
}

auto BaseNativeProvider::AddTimerController(uint32_t entity_id, float /*interval*/, bool /*repeat*/,
                                            int32_t /*user_arg*/) -> int32_t {
  ATLAS_LOG_ERROR("atlas_add_timer_controller() not supported on this process type (entity_id={})",
                  entity_id);
  return 0;
}

auto BaseNativeProvider::AddProximityController(uint32_t entity_id, float /*range*/,
                                                int32_t /*user_arg*/) -> int32_t {
  ATLAS_LOG_ERROR(
      "atlas_add_proximity_controller() not supported on this process type (entity_id={})",
      entity_id);
  return 0;
}

void BaseNativeProvider::CancelController(uint32_t entity_id, int32_t /*controller_id*/) {
  ATLAS_LOG_ERROR("atlas_cancel_controller() not supported on this process type (entity_id={})",
                  entity_id);
}

void BaseNativeProvider::ReportClientEventSeqGap(uint32_t entity_id, uint32_t /*gap_delta*/) {
  ATLAS_LOG_ERROR(
      "atlas_report_client_event_seq_gap() called on a non-client process (entity_id={})",
      entity_id);
}

}  // namespace atlas
