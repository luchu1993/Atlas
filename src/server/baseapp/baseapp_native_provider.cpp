#include "baseapp_native_provider.hpp"

#include "baseapp.hpp"
#include "foundation/log.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"
#include "network/reliable_udp.hpp"
#include "script/script_value.hpp"

#include <cstring>
#include <span>
#include <vector>

namespace atlas
{

// Packed layout of the callback table sent by C# Atlas.Runtime via set_native_callbacks.
// Must match the [UnmanagedCallersOnly] exports in Atlas.Runtime.
#pragma pack(push, 1)
struct NativeCallbackTable
{
    RestoreEntityFn restore_entity;
    GetEntityDataFn get_entity_data;
    EntityDestroyedFn entity_destroyed;
};
#pragma pack(pop)

BaseAppNativeProvider::BaseAppNativeProvider(BaseApp& app) : app_(app) {}

uint8_t BaseAppNativeProvider::get_process_prefix()
{
    return static_cast<uint8_t>(ProcessType::BaseApp);
}

void BaseAppNativeProvider::send_client_rpc(uint32_t entity_id, uint32_t rpc_id, uint8_t target,
                                            const std::byte* payload, int32_t len)
{
    auto* proxy = app_.entity_manager().find_proxy(entity_id);
    if (!proxy || !proxy->has_client())
    {
        ATLAS_LOG_WARNING("BaseApp: send_client_rpc: entity {} has no client", entity_id);
        return;
    }
    (void)proxy->client_channel()->send_message(static_cast<MessageID>(rpc_id),
                                                std::span<const std::byte>(payload, len));
    (void)target;
}

void BaseAppNativeProvider::send_cell_rpc(uint32_t entity_id, uint32_t rpc_id,
                                          const std::byte* payload, int32_t len)
{
    auto* ent = app_.entity_manager().find(entity_id);
    if (!ent || !ent->has_cell())
    {
        ATLAS_LOG_WARNING("BaseApp: send_cell_rpc: entity {} has no cell", entity_id);
        return;
    }
    // Connect to cell — nocwnd disables congestion control for this intra-DC link
    // where loss is negligible and round-trip latency is the dominant concern.
    auto cell_ch_result = app_.network().connect_rudp_nocwnd(ent->cell_addr());
    if (!cell_ch_result)
    {
        ATLAS_LOG_ERROR("BaseApp: send_cell_rpc: cannot connect to cell for entity {}", entity_id);
        return;
    }
    (void)(*cell_ch_result)
        ->send_message(static_cast<MessageID>(rpc_id), std::span<const std::byte>(payload, len));
}

void BaseAppNativeProvider::send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
                                          const std::byte* payload, int32_t len)
{
    // Base-to-base RPC: the entity lives on this BaseApp.
    // Dispatch directly via script engine callback (no network hop needed).
    std::vector<ScriptValue> args{
        ScriptValue::from_int(static_cast<int32_t>(entity_id)),
        ScriptValue::from_int(static_cast<int32_t>(rpc_id)),
        ScriptValue(std::vector<std::byte>(payload, payload + static_cast<std::size_t>(len)))};
    (void)app_.script_engine().call_function("Atlas.Runtime", "OnBaseRpc", args);
}

void BaseAppNativeProvider::write_to_db(uint32_t entity_id, const std::byte* entity_data,
                                        int32_t len)
{
    app_.do_write_to_db(entity_id, entity_data, len);
}

void BaseAppNativeProvider::give_client_to(uint32_t src_entity_id, uint32_t dest_entity_id)
{
    // If both are on this BaseApp, do local transfer.
    if (app_.entity_manager().find_proxy(dest_entity_id))
        app_.do_give_client_to_local(src_entity_id, dest_entity_id);
    else
        ATLAS_LOG_WARNING("BaseApp: give_client_to dest={} not on this BaseApp; remote case TBD",
                          dest_entity_id);
}

void BaseAppNativeProvider::set_native_callbacks(const void* native_callbacks, int32_t len)
{
    if (!native_callbacks || len < static_cast<int32_t>(sizeof(NativeCallbackTable)))
    {
        ATLAS_LOG_ERROR("BaseApp: set_native_callbacks: invalid callback table (len={})", len);
        return;
    }
    NativeCallbackTable table{};
    std::memcpy(&table, native_callbacks, sizeof(NativeCallbackTable));
    restore_entity_fn_ = table.restore_entity;
    get_entity_data_fn_ = table.get_entity_data;
    entity_destroyed_fn_ = table.entity_destroyed;
    ATLAS_LOG_INFO("BaseApp: native callback table registered");
}

}  // namespace atlas
