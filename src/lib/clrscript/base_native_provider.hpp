#pragma once

#include "clrscript/native_api_provider.hpp"

#include <cstddef>
#include <cstdint>

namespace atlas
{

// ============================================================================
// BaseNativeProvider — default INativeApiProvider implementation
// ============================================================================
//
// Provides:
//   • log_message  — routes to the Atlas logging system
//   • server_time / delta_time — returns 0 (override in concrete subclass)
//   • All RPC functions — logs an error and no-ops (not all processes support
//     every RPC direction; concrete subclasses override what they support)
//   • register_entity_type / unregister_all_entity_types — logs + no-ops
//     (subclasses connect to EntityDefRegistry when it is available)
//
// Concrete process providers (BaseAppNativeProvider, CellAppNativeProvider,
// …) inherit from this class and override only the methods they support.

class BaseNativeProvider : public INativeApiProvider
{
public:
    // ---- Logging --------------------------------------------------------
    void log_message(int32_t level, const char* msg, int32_t len) override;

    // ---- Time (stub — override in concrete provider) --------------------
    double server_time() override;
    float delta_time() override;

    // ---- Process identity (stub) ----------------------------------------
    uint8_t get_process_prefix() override;

    // ---- RPC (default: log error + no-op) --------------------------------
    void send_client_rpc(uint32_t entity_id, uint32_t rpc_id, uint8_t target,
                         const std::byte* payload, int32_t len) override;

    void send_cell_rpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                       int32_t len) override;

    void send_base_rpc(uint32_t entity_id, uint32_t rpc_id, const std::byte* payload,
                       int32_t len) override;

    // ---- Entity type registry (stub — override when EntityDef is ready) --
    void register_entity_type(const std::byte* data, int32_t len) override;
    void unregister_all_entity_types() override;

protected:
    BaseNativeProvider() = default;
};

}  // namespace atlas
