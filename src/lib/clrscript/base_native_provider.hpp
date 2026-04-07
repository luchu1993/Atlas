#pragma once

#include "clrscript/native_api_provider.hpp"
#include "foundation/log.hpp"

#include <string_view>

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

    void log_message(int32_t level, const char* msg, int32_t len) override
    {
        std::string_view message(msg, static_cast<std::size_t>(len));
        switch (static_cast<LogLevel>(level))
        {
            case LogLevel::Trace:
                ATLAS_LOG_TRACE("{}", message);
                break;
            case LogLevel::Debug:
                ATLAS_LOG_DEBUG("{}", message);
                break;
            case LogLevel::Info:
                ATLAS_LOG_INFO("{}", message);
                break;
            case LogLevel::Warning:
                ATLAS_LOG_WARNING("{}", message);
                break;
            case LogLevel::Error:
                ATLAS_LOG_ERROR("{}", message);
                break;
            case LogLevel::Critical:
                ATLAS_LOG_CRITICAL("{}", message);
                break;
            default:
                ATLAS_LOG_DEBUG("{}", message);
                break;
        }
    }

    // ---- Time (stub — override in concrete provider) --------------------

    double server_time() override { return 0.0; }
    float delta_time() override { return 0.0f; }

    // ---- Process identity (stub) ----------------------------------------

    uint8_t get_process_prefix() override
    {
        ATLAS_LOG_ERROR("get_process_prefix() not implemented for this process type");
        return 0;
    }

    // ---- RPC (default: log error + no-op) --------------------------------

    void send_client_rpc(uint32_t entity_id, uint32_t /*rpc_id*/, uint8_t /*target*/,
                         const std::byte* /*payload*/, int32_t /*len*/) override
    {
        ATLAS_LOG_ERROR(
            "send_client_rpc() not supported on this process type "
            "(entity_id={})",
            entity_id);
    }

    void send_cell_rpc(uint32_t entity_id, uint32_t /*rpc_id*/, const std::byte* /*payload*/,
                       int32_t /*len*/) override
    {
        ATLAS_LOG_ERROR(
            "send_cell_rpc() not supported on this process type "
            "(entity_id={})",
            entity_id);
    }

    void send_base_rpc(uint32_t entity_id, uint32_t /*rpc_id*/, const std::byte* /*payload*/,
                       int32_t /*len*/) override
    {
        ATLAS_LOG_ERROR(
            "send_base_rpc() not supported on this process type "
            "(entity_id={})",
            entity_id);
    }

    // ---- Entity type registry (stub — override when EntityDef is ready) --

    void register_entity_type(const std::byte* /*data*/, int32_t /*len*/) override
    {
        ATLAS_LOG_ERROR("register_entity_type() not implemented for this process type");
    }

    void unregister_all_entity_types() override
    {
        ATLAS_LOG_ERROR("unregister_all_entity_types() not implemented for this process type");
    }

protected:
    BaseNativeProvider() = default;
};

}  // namespace atlas
