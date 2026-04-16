#ifndef ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_

// ============================================================================
// ATLAS_NATIVE_API_TABLE — single source of truth for all Atlas* exports
// ============================================================================
//
// Each row: X(return_type, name, c_params, provider_call)
//
//   return_type  — C return type of the Atlas* function
//   name         — function name WITHOUT the "Atlas" prefix
//   c_params     — parameter list for the C-linkage wrapper (parenthesised)
//   provider_call — expression forwarded to INativeApiProvider (using the same
//                   parameter names as c_params)
//
// Usage examples:
//
//   // Declare all Atlas* functions (clr_native_api.hpp):
//   #define X(ret, name, params, call) ATLAS_NATIVE_API ret Atlas##name params;
//   ATLAS_NATIVE_API_TABLE(X)
//   #undef X
//
//   // Implement all Atlas* functions (clr_native_api.cpp):
//   #define X(ret, name, params, call)
//       ATLAS_NATIVE_API ret Atlas##name params { call; }
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
// NOTE: AtlasGetAbiVersion() is intentionally NOT in this table because it
// does not delegate to INativeApiProvider — it returns a compile-time constant.

// clang-format off
#define ATLAS_NATIVE_API_TABLE(X)                                                                  \
    /* ---- Logging -------------------------------------------------------- */                     \
    X(void, LogMessage,                                                                            \
        (int32_t level, const char* msg, int32_t len),                                             \
        atlas::GetNativeApiProvider().LogMessage(level, msg, len))                                  \
                                                                                                   \
    /* ---- Time ----------------------------------------------------------- */                     \
    X(double, ServerTime,                                                                          \
        (),                                                                                        \
        return atlas::GetNativeApiProvider().ServerTime())                                          \
    X(float, DeltaTime,                                                                            \
        (),                                                                                        \
        return atlas::GetNativeApiProvider().DeltaTime())                                           \
                                                                                                   \
    /* ---- Process identity ----------------------------------------------- */                     \
    X(uint8_t, GetProcessPrefix,                                                                   \
        (),                                                                                        \
        return atlas::GetNativeApiProvider().GetProcessPrefix())                                    \
                                                                                                   \
    /* ---- RPC dispatch --------------------------------------------------- */                     \
    X(void, SendClientRpc,                                                                         \
        (uint32_t entity_id, uint32_t rpc_id, uint8_t target,                                     \
         const uint8_t* payload, int32_t len),                                                     \
        atlas::GetNativeApiProvider().SendClientRpc(                                                \
            entity_id, rpc_id, target,                                                             \
            reinterpret_cast<const std::byte*>(payload), len))                                     \
    X(void, SendCellRpc,                                                                           \
        (uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len),               \
        atlas::GetNativeApiProvider().SendCellRpc(                                                  \
            entity_id, rpc_id,                                                                     \
            reinterpret_cast<const std::byte*>(payload), len))                                     \
    X(void, SendBaseRpc,                                                                           \
        (uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len),               \
        atlas::GetNativeApiProvider().SendBaseRpc(                                                  \
            entity_id, rpc_id,                                                                     \
            reinterpret_cast<const std::byte*>(payload), len))                                     \
                                                                                                   \
    /* ---- Entity type registry ------------------------------------------- */                    \
    X(void, RegisterEntityType,                                                                    \
        (const uint8_t* data, int32_t len),                                                        \
        atlas::GetNativeApiProvider().RegisterEntityType(                                           \
            reinterpret_cast<const std::byte*>(data), len))                                        \
    X(void, UnregisterAllEntityTypes,                                                              \
        (),                                                                                        \
        atlas::GetNativeApiProvider().UnregisterAllEntityTypes())                                   \
                                                                                                   \
    /* ---- Persistence ---------------------------------------------------- */                    \
    X(void, WriteToDb,                                                                             \
        (uint32_t entity_id, const uint8_t* entity_data, int32_t len),                            \
        atlas::GetNativeApiProvider().WriteToDb(                                                    \
            entity_id, reinterpret_cast<const std::byte*>(entity_data), len))                     \
                                                                                                   \
    /* ---- Client transfer ------------------------------------------------ */                    \
    X(void, GiveClientTo,                                                                          \
        (uint32_t src_entity_id, uint32_t dest_entity_id),                                        \
        atlas::GetNativeApiProvider().GiveClientTo(src_entity_id, dest_entity_id))                  \
                                                                                                   \
    /* ---- C# → C++ callback table ---------------------------------------- */                   \
    X(void, SetNativeCallbacks,                                                                    \
        (const void* native_callbacks, int32_t len),                                               \
        atlas::GetNativeApiProvider().SetNativeCallbacks(native_callbacks, len))
// clang-format on

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_
