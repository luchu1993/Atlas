#pragma once

#include "clrscript/base_native_provider.hpp"

#include <cstdint>

namespace atlas
{

class ClientApp;

// ============================================================================
// DispatchClientRpcFn — C++ calls into C# when a ClientRpc arrives from server
// ============================================================================

using ClientDispatchRpcFn = void (*)(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload,
                                     int32_t len);
using ClientCreateEntityFn = void (*)(uint32_t entity_id, uint16_t type_id);
using ClientDestroyEntityFn = void (*)(uint32_t entity_id);

// ============================================================================
// ClientNativeProvider — INativeApiProvider for the client process
// ============================================================================

class ClientNativeProvider : public BaseNativeProvider
{
public:
    explicit ClientNativeProvider(ClientApp& app);

    uint8_t get_process_prefix() override;

    // Client sends exposed RPCs to server via BaseApp
    void send_base_rpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                       int32_t len) override;
    void send_cell_rpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                       int32_t len) override;

    // C# callback table registration
    void set_native_callbacks(const void* native_callbacks, int32_t len) override;

    // Callback accessors
    [[nodiscard]] auto dispatch_rpc_fn() const -> ClientDispatchRpcFn { return dispatch_rpc_fn_; }
    [[nodiscard]] auto create_entity_fn() const -> ClientCreateEntityFn
    {
        return create_entity_fn_;
    }
    [[nodiscard]] auto destroy_entity_fn() const -> ClientDestroyEntityFn
    {
        return destroy_entity_fn_;
    }

private:
    ClientApp& app_;
    ClientDispatchRpcFn dispatch_rpc_fn_{nullptr};
    ClientCreateEntityFn create_entity_fn_{nullptr};
    ClientDestroyEntityFn destroy_entity_fn_{nullptr};
};

}  // namespace atlas
