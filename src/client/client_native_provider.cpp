#include "client_native_provider.hpp"

#include "baseapp/baseapp_messages.hpp"
#include "client_app.hpp"
#include "foundation/log.hpp"
#include "network/channel.hpp"
#include "network/message_ids.hpp"

#include <cstring>

namespace atlas
{

// Packed layout of the callback table sent by C# Atlas.Client via set_native_callbacks.
#pragma pack(push, 1)
struct ClientCallbackTable
{
    ClientDispatchRpcFn dispatch_rpc;
    ClientCreateEntityFn create_entity;
    ClientDestroyEntityFn destroy_entity;
};
#pragma pack(pop)

ClientNativeProvider::ClientNativeProvider(ClientApp& app) : app_(app) {}

uint8_t ClientNativeProvider::get_process_prefix()
{
    // 'C' for Client
    return static_cast<uint8_t>('C');
}

void ClientNativeProvider::send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
                                         const std::byte* payload, int32_t len)
{
    auto* ch = app_.baseapp_channel();
    if (!ch)
    {
        ATLAS_LOG_WARNING("Client: send_base_rpc: not connected to BaseApp");
        return;
    }

    // Build and send ClientBaseRpc message
    baseapp::ClientBaseRpc msg;
    msg.rpc_id = rpc_id;
    if (len > 0)
        msg.payload.assign(payload, payload + static_cast<std::size_t>(len));

    (void)ch->send_message(msg);

    (void)entity_id;  // entity_id is implicit (proxy-bound)
}

void ClientNativeProvider::send_cell_rpc(uint32_t entity_id, uint32_t rpc_id,
                                         const std::byte* payload, int32_t len)
{
    // Phase 2: ClientCellRpc forwarding to CellApp via BaseApp
    ATLAS_LOG_WARNING("Client: send_cell_rpc not yet implemented (rpc_id=0x{:06X})", rpc_id);
    (void)entity_id;
    (void)payload;
    (void)len;
}

void ClientNativeProvider::set_native_callbacks(const void* native_callbacks, int32_t len)
{
    if (!native_callbacks || len < static_cast<int32_t>(sizeof(ClientCallbackTable)))
    {
        ATLAS_LOG_ERROR("Client: set_native_callbacks: invalid callback table (len={})", len);
        return;
    }
    ClientCallbackTable table{};
    std::memcpy(&table, native_callbacks, sizeof(ClientCallbackTable));
    dispatch_rpc_fn_ = table.dispatch_rpc;
    create_entity_fn_ = table.create_entity;
    destroy_entity_fn_ = table.destroy_entity;
    ATLAS_LOG_INFO("Client: native callback table registered");
}

}  // namespace atlas
