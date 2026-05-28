#include "client_native_provider.h"

#include <cmath>
#include <cstring>
#include <span>

#include "baseapp/baseapp_messages.h"
#include "client_app.h"
#include "foundation/log.h"
#include "foundation/process_type.h"
#include "movement_sim/movement_codec.h"
#include "network/channel.h"
#include "serialization/binary_stream.h"
#include "server/entity_types.h"

namespace atlas {

namespace {

auto IsValidNativePayload(const std::byte* payload, int32_t len) -> bool {
  return len >= 0 && (payload != nullptr || len == 0);
}

}  // namespace

// Mirrors [UnmanagedCallersOnly] exports in Atlas.Client. Append-only.
#pragma pack(push, 1)
struct ClientCallbackTable {
  ClientDispatchRpcFn dispatch_rpc;
  ClientCreateEntityFn create_entity;
  ClientDestroyEntityFn destroy_entity;
  ClientDeliverFromServerFn deliver_from_server;
};
#pragma pack(pop)

ClientNativeProvider::ClientNativeProvider(ClientApp& app) : app_(app) {}

uint8_t ClientNativeProvider::GetProcessPrefix() {
  return static_cast<uint8_t>(ProcessType::kClient);
}

void ClientNativeProvider::SendBaseRpc(uint32_t /*entity_id*/, uint32_t rpc_id,
                                       const std::byte* payload, int32_t len, uint64_t trace_id) {
  if (!IsValidNativePayload(payload, len)) {
    ATLAS_LOG_WARNING("Client: send_base_rpc rejected invalid payload len={}", len);
    return;
  }
  auto* ch = app_.BaseappChannel();
  if (!ch) {
    ATLAS_LOG_WARNING("Client: send_base_rpc: not connected to BaseApp");
    return;
  }

  // entity_id is implicit on the proxy-bound channel.
  baseapp::ClientBaseRpc msg;
  msg.rpc_id = rpc_id;
  msg.trace_id = trace_id;
  if (len > 0) msg.payload.assign(payload, payload + static_cast<std::size_t>(len));

  (void)ch->SendMessage(msg);
}

void ClientNativeProvider::SendCellRpc(uint32_t entity_id, uint32_t rpc_id,
                                       const std::byte* payload, int32_t len, uint64_t trace_id) {
  if (!IsValidNativePayload(payload, len)) {
    ATLAS_LOG_WARNING("Client: send_cell_rpc rejected invalid payload len={}", len);
    return;
  }
  auto* ch = app_.BaseappChannel();
  if (!ch) {
    ATLAS_LOG_WARNING("Client: send_cell_rpc: not connected to BaseApp");
    return;
  }

  baseapp::ClientCellRpc msg;
  msg.target_entity_id = entity_id;
  msg.rpc_id = rpc_id;
  msg.trace_id = trace_id;
  if (len > 0) msg.payload.assign(payload, payload + static_cast<std::size_t>(len));

  (void)ch->SendMessage(msg);
}

void ClientNativeProvider::SendMovementInput(uint32_t target_entity_id, const std::byte* frames,
                                             int32_t frame_count) {
  if (target_entity_id == kInvalidEntityID || frames == nullptr || frame_count <= 0 ||
      frame_count > static_cast<int32_t>(movement::kMaxMovementInputFrames)) {
    ATLAS_LOG_WARNING("Client: send_movement_input rejected invalid args target={} count={}",
                      target_entity_id, frame_count);
    return;
  }
  auto* ch = app_.BaseappChannel();
  if (!ch) {
    ATLAS_LOG_WARNING("Client: send_movement_input: not connected to BaseApp");
    return;
  }

  const auto count = static_cast<std::size_t>(frame_count);
  std::span<const std::byte> payload{frames, count * movement::kInputFrameWireBytes};
  BinaryReader reader(payload);
  baseapp::ClientMovementInput msg;
  msg.target_entity_id = target_entity_id;
  msg.frames.reserve(count);
  for (int32_t i = 0; i < frame_count; ++i) {
    auto frame = movement::DeserializeInputFrame(reader);
    if (!frame || !movement::IsInputFrameValid(*frame)) {
      ATLAS_LOG_WARNING("Client: send_movement_input rejected invalid frame at index={}", i);
      return;
    }
    msg.frames.push_back(*frame);
  }

  if (auto result = ch->SendMessage(msg); !result) {
    ATLAS_LOG_WARNING("Client: send_movement_input failed: {}", result.Error().Message());
  }
}

void ClientNativeProvider::SendMovementCorrectionReport(uint32_t target_entity_id,
                                                        uint32_t acked_input_seq,
                                                        uint32_t server_tick, float distance_m,
                                                        uint16_t correction_flags) {
  constexpr uint16_t kValidFlags = movement::kCorrectionFlagTier1 |
                                   movement::kCorrectionFlagTier2 |
                                   movement::kCorrectionFlagSnap;
  if (target_entity_id == kInvalidEntityID || !std::isfinite(distance_m) ||
      distance_m < 0.0f || (correction_flags & ~kValidFlags) != 0) {
    ATLAS_LOG_WARNING(
        "Client: send_movement_correction_report rejected invalid args target={} "
        "distance={} flags={}",
        target_entity_id, distance_m, correction_flags);
    return;
  }
  auto* ch = app_.BaseappChannel();
  if (!ch) {
    ATLAS_LOG_WARNING("Client: send_movement_correction_report: not connected to BaseApp");
    return;
  }

  baseapp::MovementCorrectionReport msg;
  msg.target_entity_id = target_entity_id;
  msg.acked_input_seq = acked_input_seq;
  msg.server_tick = server_tick;
  msg.distance_m = distance_m;
  msg.correction_flags = correction_flags;
  if (auto result = ch->SendMessage(msg); !result) {
    ATLAS_LOG_WARNING("Client: send_movement_correction_report failed: {}",
                      result.Error().Message());
  }
}

void ClientNativeProvider::ReportClientEventSeqGap(uint32_t entity_id, uint32_t gap_delta) {
  if (gap_delta == 0) return;
  auto* ch = app_.BaseappChannel();
  // Channel may be torn down during shutdown; drop the report silently.
  if (ch == nullptr) return;
  baseapp::ClientEventSeqReport msg;
  msg.entity_id = entity_id;
  msg.gap_delta = gap_delta;
  (void)ch->SendMessage(msg);
}

void ClientNativeProvider::SetNativeCallbacks(const void* native_callbacks, int32_t len) {
  if (!native_callbacks || len < static_cast<int32_t>(sizeof(ClientCallbackTable))) {
    ATLAS_LOG_ERROR("Client: set_native_callbacks: invalid callback table (len={})", len);
    return;
  }
  ClientCallbackTable table{};
  std::memcpy(&table, native_callbacks, sizeof(ClientCallbackTable));
  dispatch_rpc_fn_ = table.dispatch_rpc;
  create_entity_fn_ = table.create_entity;
  destroy_entity_fn_ = table.destroy_entity;
  deliver_from_server_fn_ = table.deliver_from_server;
  ATLAS_LOG_INFO("Client: native callback table registered");
}

}  // namespace atlas
