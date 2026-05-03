#include "clrscript/base_native_provider.h"

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
                                       RpcTarget /*target*/, const std::byte* /*payload*/,
                                       int32_t /*len*/) {
  ATLAS_LOG_ERROR("send_client_rpc() not supported on this process type (entity_id={})", entity_id);
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

void BaseNativeProvider::RegisterStruct(const std::byte* data, int32_t len) {
  EntityDefRegistry::Instance().RegisterStruct(data, len);
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

void BaseNativeProvider::SetNativeCallbacks(const void* /*native_callbacks*/, int32_t /*len*/) {}

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
