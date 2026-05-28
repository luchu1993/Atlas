#include "clrscript/base_native_provider.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

#include "coro/entity_rpc_reply.h"
#include "entitydef/entity_def_registry.h"
#include "foundation/log.h"
#include "network/channel.h"

namespace atlas {

void BaseNativeProvider::LogMessage(int32_t level, const char* msg, int32_t len) {
  if (msg == nullptr || len <= 0) return;

  std::string_view message(msg, static_cast<std::size_t>(len));
  auto log_level = static_cast<LogLevel>(level);
  auto& logger = Logger::Instance();
  if (static_cast<uint8_t>(log_level) >= static_cast<uint8_t>(logger.Level()))
    logger.Log(log_level, "clr", message);
}

double BaseNativeProvider::ServerTime() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

float BaseNativeProvider::DeltaTime() {
  return 0.0f;
}

uint8_t BaseNativeProvider::GetProcessPrefix() {
  ATLAS_LOG_ERROR("get_process_prefix() not implemented for this process type");
  return 0;
}

void BaseNativeProvider::SendClientRpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                       RpcTarget /*target*/, const std::byte* /*payload*/,
                                       int32_t /*len*/, uint64_t /*trace_id*/) {
  ATLAS_LOG_ERROR("send_client_rpc() not supported on this process type (entity_id={})", entity_id);
}

void BaseNativeProvider::SendCellRpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                     const std::byte* /*payload*/, int32_t /*len*/,
                                     uint64_t /*trace_id*/) {
  ATLAS_LOG_ERROR(
      "send_cell_rpc() not supported on this process type "
      "(entity_id={})",
      entity_id);
}

void BaseNativeProvider::SendBaseRpc(uint32_t entity_id, uint32_t /*rpc_id*/,
                                     const std::byte* /*payload*/, int32_t /*len*/,
                                     uint64_t /*trace_id*/) {
  ATLAS_LOG_ERROR(
      "send_base_rpc() not supported on this process type "
      "(entity_id={})",
      entity_id);
}

void BaseNativeProvider::SendMovementInput(uint32_t target_entity_id,
                                           const std::byte* /*frames*/,
                                           int32_t /*frame_count*/) {
  ATLAS_LOG_ERROR(
      "send_movement_input() not supported on this process type "
      "(target_entity_id={})",
      target_entity_id);
}

void BaseNativeProvider::SendMovementCorrectionReport(uint32_t target_entity_id,
                                                      uint32_t /*acked_input_seq*/,
                                                      uint32_t /*server_tick*/,
                                                      float /*distance_m*/,
                                                      uint16_t /*correction_flags*/) {
  ATLAS_LOG_ERROR(
      "send_movement_correction_report() not supported on this process type "
      "(target_entity_id={})",
      target_entity_id);
}

void BaseNativeProvider::RegisterEntityType(const std::byte* data, int32_t len) {
  EntityDefRegistry::Instance().RegisterType(data, len);
}

void BaseNativeProvider::UnregisterAllEntityTypes() {
  EntityDefRegistry::Instance().clear();
}

void BaseNativeProvider::RegisterStruct(const std::byte* data, int32_t len) {
  EntityDefRegistry::Instance().RegisterStruct(data, len);
}

void BaseNativeProvider::RegisterComponent(const std::byte* data, int32_t len) {
  EntityDefRegistry::Instance().RegisterComponent(data, len);
}

void BaseNativeProvider::SetEntityDefDigest(const std::byte* data, int32_t len) {
  EntityDefRegistry::Instance().SetDigest(data, len);
}

void BaseNativeProvider::WriteToDb(uint32_t entity_id, const std::byte* /*entity_data*/,
                                   int32_t /*len*/) {
  ATLAS_LOG_ERROR("write_to_db() not supported on this process type (entity_id={})", entity_id);
}

void BaseNativeProvider::GiveClientTo(uint32_t src_entity_id, uint32_t /*dest_entity_id*/) {
  ATLAS_LOG_ERROR("give_client_to() not supported on this process type (src={})", src_entity_id);
}

void BaseNativeProvider::SetSpaceMasterType(uint32_t /*space_id*/, const char* /*name*/,
                                            int32_t /*len*/) {
  // Default no-op; only BaseAppNativeProvider routes to BaseApp::SetSpaceMasterType.
}

auto BaseNativeProvider::CreateBaseEntity(uint16_t type_id, uint32_t /*space_id*/) -> uint32_t {
  ATLAS_LOG_ERROR("create_base_entity() not supported on this process type (type_id={})", type_id);
  return 0;
}

auto BaseNativeProvider::CreateLocalCellEntity(uint16_t type_id, uint32_t /*space_id*/,
                                               float /*pos_x*/, float /*pos_y*/, float /*pos_z*/,
                                               float /*dir_x*/, float /*dir_y*/, float /*dir_z*/,
                                               bool /*on_ground*/) -> uint32_t {
  ATLAS_LOG_ERROR("create_local_cell_entity() only supported on CellApp (type_id={})", type_id);
  return 0;
}

void BaseNativeProvider::DestroyCellEntity(uint32_t entity_id) {
  ATLAS_LOG_ERROR("destroy_cell_entity() only supported on CellApp (entity_id={})", entity_id);
}

auto BaseNativeProvider::RequestSpawnCellOnly(uint16_t type_id, uint32_t /*space_id*/,
                                              float /*pos_x*/, float /*pos_y*/, float /*pos_z*/,
                                              float /*dir_x*/, float /*dir_y*/, float /*dir_z*/,
                                              bool /*on_ground*/) -> bool {
  ATLAS_LOG_ERROR("request_spawn_cell_only() only supported on BaseApp (type_id={})", type_id);
  return false;
}

void BaseNativeProvider::SetAoIRadius(uint32_t entity_id, float /*radius*/, float /*hysteresis*/) {
  ATLAS_LOG_ERROR("set_aoi_radius() not supported on this process type (entity_id={})", entity_id);
}

void BaseNativeProvider::SetSpaceData(uint32_t space_id, uint16_t /*key_id*/,
                                      const std::byte* /*value*/, int32_t /*len*/) {
  ATLAS_LOG_ERROR("set_space_data() not supported on this process type (space_id={})", space_id);
}

void BaseNativeProvider::RemoveSpaceData(uint32_t space_id, uint16_t /*key_id*/) {
  ATLAS_LOG_ERROR("remove_space_data() not supported on this process type (space_id={})", space_id);
}

auto BaseNativeProvider::LoadCollisionAsset(uint32_t space_id, const char* /*path*/,
                                            int32_t /*len*/) -> bool {
  ATLAS_LOG_ERROR("load_collision_asset() not supported on this process type (space_id={})",
                  space_id);
  return false;
}

auto BaseNativeProvider::GetEntitySpaceId(uint32_t /*entity_id*/) -> uint32_t { return 0; }

void BaseNativeProvider::SetNativeCallbacks(const void* /*native_callbacks*/, int32_t /*len*/) {}

void BaseNativeProvider::SetEntityPosition(uint32_t entity_id, float /*x*/, float /*y*/,
                                           float /*z*/) {
  ATLAS_LOG_ERROR("atlas_set_position() not supported on this process type (entity_id={})",
                  entity_id);
}

void BaseNativeProvider::SetEntityDirection(uint32_t entity_id, float /*x*/, float /*y*/,
                                            float /*z*/) {
  ATLAS_LOG_ERROR("atlas_set_direction() not supported on this process type (entity_id={})",
                  entity_id);
}

void BaseNativeProvider::SetEntityOnGround(uint32_t entity_id, bool /*on_ground*/) {
  ATLAS_LOG_ERROR("atlas_set_on_ground() not supported on this process type (entity_id={})",
                  entity_id);
}

void BaseNativeProvider::SetMovementIntent(uint32_t entity_id, float /*dir_x*/, float /*dir_z*/,
                                           float /*speed_mps*/, uint16_t /*buttons*/) {
  ATLAS_LOG_ERROR("atlas_set_movement_intent() not supported on this process type (entity_id={})",
                  entity_id);
}

auto BaseNativeProvider::SetMovementCommand(uint32_t entity_id,
                                            const NativeMovementCommand& /*command*/) -> bool {
  ATLAS_LOG_ERROR("atlas_set_movement_command() not supported on this process type "
                  "(entity_id={})",
                  entity_id);
  return false;
}

auto BaseNativeProvider::ClearMovementCommand(uint32_t entity_id,
                                              uint32_t /*command_id*/) -> bool {
  ATLAS_LOG_ERROR("atlas_clear_movement_command() not supported on this process type "
                  "(entity_id={})",
                  entity_id);
  return false;
}

auto BaseNativeProvider::SetMovementCurve(const NativeMovementCurve& curve) -> bool {
  ATLAS_LOG_ERROR("atlas_set_movement_curve() not supported on this process type "
                  "(curve_id={})",
                  curve.curve_id);
  return false;
}

void BaseNativeProvider::GetEntityPosition(uint32_t entity_id, float& x, float& y, float& z) {
  ATLAS_LOG_ERROR("atlas_get_position() not supported on this process type (entity_id={})",
                  entity_id);
  x = 0;
  y = 0;
  z = 0;
}

void BaseNativeProvider::GetEntityDirection(uint32_t entity_id, float& x, float& y, float& z) {
  ATLAS_LOG_ERROR("atlas_get_direction() not supported on this process type (entity_id={})",
                  entity_id);
  x = 0;
  y = 0;
  z = 0;
}

auto BaseNativeProvider::GetEntityOnGround(uint32_t entity_id) -> bool {
  ATLAS_LOG_ERROR("atlas_get_on_ground() not supported on this process type (entity_id={})",
                  entity_id);
  return false;
}

auto BaseNativeProvider::TryGetMovementHistorySample(
    uint32_t entity_id, uint32_t /*server_tick*/, NativeMovementHistorySample& sample) -> bool {
  ATLAS_LOG_ERROR("atlas_try_get_movement_history() not supported on this process type "
                  "(entity_id={})",
                  entity_id);
  sample = {};
  return false;
}

void BaseNativeProvider::PublishReplicationFrame(
    uint32_t entity_id, bool /*has_event*/, bool /*has_volatile*/,
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

void BaseNativeProvider::SendEntityRpcSuccess(intptr_t reply_channel, uint32_t request_id,
                                              const std::byte* body, int32_t len) {
  if (reply_channel == 0) return;  // in-process call, no remote to reply to
  auto* ch = reinterpret_cast<Channel*>(reply_channel);
  std::span<const std::byte> body_span =
      (body != nullptr && len > 0) ? std::span<const std::byte>{body, static_cast<size_t>(len)}
                                   : std::span<const std::byte>{};
  if (auto r = entity_rpc_reply::SendSuccess(*ch, request_id, body_span); !r) {
    ATLAS_LOG_WARNING("SendEntityRpcSuccess: send failed (req={}, err={})", request_id,
                      r.Error().Message());
  }
}

void BaseNativeProvider::SendEntityRpcFailure(intptr_t reply_channel, uint32_t request_id,
                                              int32_t error_code, const char* msg,
                                              int32_t msg_len) {
  if (reply_channel == 0) return;
  auto* ch = reinterpret_cast<Channel*>(reply_channel);
  std::string_view msg_view{msg != nullptr ? msg : "",
                            msg_len > 0 ? static_cast<size_t>(msg_len) : 0};
  if (auto r = entity_rpc_reply::SendFailure(*ch, request_id, error_code, msg_view); !r) {
    ATLAS_LOG_WARNING("SendEntityRpcFailure: send failed (req={}, code={}, err={})", request_id,
                      error_code, r.Error().Message());
  }
}

}  // namespace atlas
