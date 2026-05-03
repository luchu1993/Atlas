#include "client_native_provider.h"

#include <cstring>

#include "baseapp/baseapp_messages.h"
#include "client_app.h"
#include "foundation/log.h"
#include "network/channel.h"

namespace atlas {

namespace {

auto IsValidNativePayload(const std::byte* payload, int32_t len) -> bool {
  return len >= 0 && (payload != nullptr || len == 0);
}

}  // namespace

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
  return static_cast<uint8_t>('C');
}

void ClientNativeProvider::SendBaseRpc(uint32_t entity_id, uint32_t rpc_id,
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

  baseapp::ClientBaseRpc msg;
  msg.rpc_id = rpc_id;
  msg.trace_id = trace_id;
  if (len > 0) msg.payload.assign(payload, payload + static_cast<std::size_t>(len));

  (void)ch->SendMessage(msg);

  (void)entity_id;  // entity_id is implicit (proxy-bound)
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

void ClientNativeProvider::ReportClientEventSeqGap(uint32_t entity_id, uint32_t gap_delta) {
  if (gap_delta == 0) return;
  auto* ch = app_.BaseappChannel();
  if (ch == nullptr) {
    // Channel may be torn down during shutdown — drop the report silently.
    return;
  }
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
