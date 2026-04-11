#pragma once

// ============================================================================
// ATLAS_NATIVE_API_TABLE — single source of truth for all atlas_* exports
// ============================================================================
//
// Each row: X(return_type, name, c_params, provider_call)
//
//   return_type  — C return type of the atlas_* function
//   name         — function name WITHOUT the "atlas_" prefix
//   c_params     — parameter list for the C-linkage wrapper (parenthesised)
//   provider_call — expression forwarded to INativeApiProvider (using the same
//                   parameter names as c_params)
//
// Usage examples:
//
//   // Declare all atlas_* functions (clr_native_api.hpp):
//   #define X(ret, name, params, call) ATLAS_NATIVE_API ret atlas_##name params;
//   ATLAS_NATIVE_API_TABLE(X)
//   #undef X
//
//   // Implement all atlas_* functions (clr_native_api.cpp):
//   #define X(ret, name, params, call)
//       ATLAS_NATIVE_API ret atlas_##name params { call; }
//   ATLAS_NATIVE_API_TABLE(X)
//   #undef X
//
//   // Declare all pure-virtual methods (native_api_provider.hpp):
//   #define X(ret, name, params, call) virtual ret name params = 0;
//   ATLAS_NATIVE_API_TABLE(X)
//   #undef X
//
// When adding a new API, edit ONLY this file.  All four files that consume the
// table (clr_native_api.hpp, clr_native_api.cpp, native_api_provider.hpp,
// base_native_provider.hpp/.cpp) update automatically.
//
// NOTE: atlas_get_abi_version() is intentionally NOT in this table because it
// does not delegate to INativeApiProvider — it returns a compile-time constant.

// clang-format off
#define ATLAS_NATIVE_API_TABLE(X)                                                                  \
    /* ---- Logging -------------------------------------------------------- */                     \
    X(void, log_message,                                                                           \
        (int32_t level, const char* msg, int32_t len),                                             \
        atlas::get_native_api_provider().log_message(level, msg, len))                            \
                                                                                                   \
    /* ---- Time ----------------------------------------------------------- */                     \
    X(double, server_time,                                                                         \
        (),                                                                                        \
        return atlas::get_native_api_provider().server_time())                                     \
    X(float, delta_time,                                                                           \
        (),                                                                                        \
        return atlas::get_native_api_provider().delta_time())                                      \
                                                                                                   \
    /* ---- Process identity ----------------------------------------------- */                     \
    X(uint8_t, get_process_prefix,                                                                 \
        (),                                                                                        \
        return atlas::get_native_api_provider().get_process_prefix())                              \
                                                                                                   \
    /* ---- RPC dispatch --------------------------------------------------- */                     \
    X(void, send_client_rpc,                                                                       \
        (uint32_t entity_id, uint32_t rpc_id, uint8_t target,                                     \
         const uint8_t* payload, int32_t len),                                                     \
        atlas::get_native_api_provider().send_client_rpc(                                          \
            entity_id, rpc_id, target,                                                             \
            reinterpret_cast<const std::byte*>(payload), len))                                     \
    X(void, send_cell_rpc,                                                                         \
        (uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len),               \
        atlas::get_native_api_provider().send_cell_rpc(                                            \
            entity_id, rpc_id,                                                                     \
            reinterpret_cast<const std::byte*>(payload), len))                                     \
    X(void, send_base_rpc,                                                                         \
        (uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len),               \
        atlas::get_native_api_provider().send_base_rpc(                                            \
            entity_id, rpc_id,                                                                     \
            reinterpret_cast<const std::byte*>(payload), len))                                     \
                                                                                                   \
    /* ---- Entity type registry ------------------------------------------- */                    \
    X(void, register_entity_type,                                                                  \
        (const uint8_t* data, int32_t len),                                                        \
        atlas::get_native_api_provider().register_entity_type(                                     \
            reinterpret_cast<const std::byte*>(data), len))                                        \
    X(void, unregister_all_entity_types,                                                           \
        (),                                                                                        \
        atlas::get_native_api_provider().unregister_all_entity_types())
// clang-format on
