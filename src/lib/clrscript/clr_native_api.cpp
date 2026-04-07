#include "clrscript/clr_native_api.hpp"

#include "clrscript/native_api_provider.hpp"

#include <cstddef>

// ============================================================================
// atlas_* export function implementations
// ============================================================================
//
// This file is compiled only into the atlas_engine shared library
// (ATLAS_ENGINE_EXPORTS is defined for that target).  Each function is a
// one-line delegation to the INativeApiProvider registered at startup.

// ---- Logging ----------------------------------------------------------------

ATLAS_NATIVE_API void atlas_log_message(int32_t level, const char* msg, int32_t len)
{
    atlas::get_native_api_provider().log_message(level, msg, len);
}

// ---- Time -------------------------------------------------------------------

ATLAS_NATIVE_API double atlas_server_time()
{
    return atlas::get_native_api_provider().server_time();
}

ATLAS_NATIVE_API float atlas_delta_time()
{
    return atlas::get_native_api_provider().delta_time();
}

// ---- Process identity -------------------------------------------------------

ATLAS_NATIVE_API uint8_t atlas_get_process_prefix()
{
    return atlas::get_native_api_provider().get_process_prefix();
}

// ---- RPC dispatch -----------------------------------------------------------

ATLAS_NATIVE_API void atlas_send_client_rpc(uint32_t entity_id, uint32_t rpc_id, uint8_t target,
                                            const uint8_t* payload, int32_t len)
{
    atlas::get_native_api_provider().send_client_rpc(
        entity_id, rpc_id, target, reinterpret_cast<const std::byte*>(payload), len);
}

ATLAS_NATIVE_API void atlas_send_cell_rpc(uint32_t entity_id, uint32_t rpc_id,
                                          const uint8_t* payload, int32_t len)
{
    atlas::get_native_api_provider().send_cell_rpc(
        entity_id, rpc_id, reinterpret_cast<const std::byte*>(payload), len);
}

ATLAS_NATIVE_API void atlas_send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
                                          const uint8_t* payload, int32_t len)
{
    atlas::get_native_api_provider().send_base_rpc(
        entity_id, rpc_id, reinterpret_cast<const std::byte*>(payload), len);
}

// ---- Entity type registry ---------------------------------------------------

ATLAS_NATIVE_API void atlas_register_entity_type(const uint8_t* data, int32_t len)
{
    atlas::get_native_api_provider().register_entity_type(reinterpret_cast<const std::byte*>(data),
                                                          len);
}

ATLAS_NATIVE_API void atlas_unregister_all_entity_types()
{
    atlas::get_native_api_provider().unregister_all_entity_types();
}
