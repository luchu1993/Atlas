#include "baseapp_native_provider.h"

#include <algorithm>
#include <cstring>
#include <span>

#include "base_entity.h"
#include "baseapp.h"
#include "cellapp/cellapp_messages.h"
#include "foundation/log.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "network/reliable_udp.h"

namespace atlas {

// Packed layout of the callback table sent by C# Atlas.Runtime via set_native_callbacks.
// Must match the [UnmanagedCallersOnly] exports in Atlas.Runtime.
// New entries are appended at the end; C++ tolerates short tables (older runtimes).
#pragma pack(push, 1)
struct NativeCallbackTable {
  RestoreEntityFn restore_entity;
  GetEntityDataFn get_entity_data;
  EntityDestroyedFn entity_destroyed;
  DispatchRpcFn dispatch_rpc;
  GetOwnerSnapshotFn get_owner_snapshot;  // appended — optional for older runtimes
};
#pragma pack(pop)

BaseAppNativeProvider::BaseAppNativeProvider(BaseApp& app) : app_(app) {}

uint8_t BaseAppNativeProvider::GetProcessPrefix() {
  return static_cast<uint8_t>(ProcessType::kBaseApp);
}

void BaseAppNativeProvider::SendClientRpc(uint32_t entity_id, uint32_t rpc_id,
                                          const std::byte* payload, int32_t len) {
  auto* proxy = app_.GetEntityManager().FindProxy(entity_id);
  if (!proxy || !proxy->HasClient()) {
    ATLAS_LOG_WARNING("BaseApp: SendClientRpc: entity {} has no client", entity_id);
    return;
  }
  auto* client_ch = app_.ResolveClientChannel(entity_id);
  if (!client_ch) {
    ATLAS_LOG_WARNING("BaseApp: SendClientRpc: entity {} client channel unavailable", entity_id);
    return;
  }
  // Unified envelope (see RelayRpcToClient).
  std::vector<std::byte> tmp(payload, payload + len);
  app_.RelayRpcToClient(*client_ch, rpc_id, tmp);
}

void BaseAppNativeProvider::SendCellRpc(uint32_t entity_id, uint32_t rpc_id,
                                        const std::byte* payload, int32_t len) {
  auto* ent = app_.GetEntityManager().Find(entity_id);
  if (!ent || !ent->HasCell()) {
    ATLAS_LOG_WARNING("BaseApp: SendCellRpc: entity {} has no cell", entity_id);
    return;
  }
  // Connect to cell — nocwnd disables congestion control for this intra-DC link
  // where loss is negligible and round-trip latency is the dominant concern.
  auto cell_ch_result = app_.Network().ConnectRudpNocwnd(ent->CellAddr());
  if (!cell_ch_result) {
    ATLAS_LOG_ERROR("BaseApp: SendCellRpc: cannot connect to cell for entity {}", entity_id);
    return;
  }
  // Wrap in cellapp::InternalCellRpc so the cell side dispatches via
  // its registered typed handler (CellApp::OnInternalCellRpc).  The
  // protocol-level MessageID stays in the cellapp/ID space (3004); the
  // application-level rpc_id rides in the body.
  cellapp::InternalCellRpc msg;
  msg.target_entity_id = entity_id;
  msg.rpc_id = rpc_id;
  if (len > 0) msg.payload.assign(payload, payload + static_cast<std::size_t>(len));
  if (auto r = (*cell_ch_result)->SendMessage(msg); !r) {
    ATLAS_LOG_DEBUG("BaseApp: SendCellRpc send failed (entity={}, rpc_id=0x{:08X}): {}", entity_id,
                    rpc_id, r.Error().Message());
  }
}

void BaseAppNativeProvider::SendBaseRpc(uint32_t entity_id, uint32_t rpc_id,
                                        const std::byte* payload, int32_t len) {
  // Base-to-base RPC: the entity lives on this BaseApp.
  // Dispatch directly via the C# callback (no network hop needed).
  if (!dispatch_rpc_fn_) {
    ATLAS_LOG_WARNING("BaseApp: SendBaseRpc: dispatch_rpc callback not registered");
    return;
  }
  dispatch_rpc_fn_(entity_id, rpc_id, reinterpret_cast<const uint8_t*>(payload), len);
}

void BaseAppNativeProvider::WriteToDb(uint32_t entity_id, const std::byte* entity_data,
                                      int32_t len) {
  app_.DoWriteToDb(entity_id, entity_data, len);
}

void BaseAppNativeProvider::GiveClientTo(uint32_t src_entity_id, uint32_t dest_entity_id) {
  // If both are on this BaseApp, do local transfer.
  if (app_.GetEntityManager().FindProxy(dest_entity_id))
    app_.DoGiveClientToLocal(src_entity_id, dest_entity_id);
  else
    ATLAS_LOG_WARNING("BaseApp: GiveClientTo dest={} not on this BaseApp; remote case TBD",
                      dest_entity_id);
}

auto BaseAppNativeProvider::CreateBaseEntity(uint16_t type_id, uint32_t space_id) -> uint32_t {
  return app_.CreateBaseEntityFromScript(type_id, space_id);
}

void BaseAppNativeProvider::SetAoIRadius(uint32_t entity_id, float radius, float hysteresis) {
  auto* ent = app_.GetEntityManager().Find(entity_id);
  if (!ent) {
    ATLAS_LOG_WARNING("BaseApp: SetAoIRadius on unknown entity_id={}", entity_id);
    return;
  }
  if (!ent->HasCell()) {
    ATLAS_LOG_WARNING(
        "BaseApp: SetAoIRadius on entity_id={} with no cell counterpart — "
        "call from a has_cell entity after the cell ack has landed",
        entity_id);
    return;
  }
  auto* cell_ch = app_.ResolveCellChannelForEntity(entity_id);
  if (!cell_ch) {
    ATLAS_LOG_WARNING("BaseApp: SetAoIRadius on entity_id={} but cell channel unresolved",
                      entity_id);
    return;
  }
  cellapp::SetAoIRadius msg;
  msg.base_entity_id = entity_id;
  msg.radius = radius;
  msg.hysteresis = hysteresis;
  (void)cell_ch->SendMessage(msg);
}

void BaseAppNativeProvider::SetNativeCallbacks(const void* native_callbacks, int32_t len) {
  // Minimum accepted size = the original 4-entry table; new entries are
  // optional so pre-baseline runtimes still register cleanly.
  constexpr int32_t kMinTableBytes =
      static_cast<int32_t>(sizeof(RestoreEntityFn) + sizeof(GetEntityDataFn) +
                           sizeof(EntityDestroyedFn) + sizeof(DispatchRpcFn));
  if (!native_callbacks || len < kMinTableBytes) {
    ATLAS_LOG_ERROR("BaseApp: set_native_callbacks: invalid callback table (len={})", len);
    return;
  }
  NativeCallbackTable table{};
  const auto copy_bytes = std::min<int32_t>(len, static_cast<int32_t>(sizeof(NativeCallbackTable)));
  std::memcpy(&table, native_callbacks, static_cast<size_t>(copy_bytes));
  restore_entity_fn_ = table.restore_entity;
  get_entity_data_fn_ = table.get_entity_data;
  entity_destroyed_fn_ = table.entity_destroyed;
  dispatch_rpc_fn_ = table.dispatch_rpc;
  get_owner_snapshot_fn_ = table.get_owner_snapshot;  // nullptr if runtime predates baseline
  ATLAS_LOG_INFO("BaseApp: native callback table registered (len={})", len);
}

}  // namespace atlas
