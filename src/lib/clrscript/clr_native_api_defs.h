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
        (uint32_t entity_id, uint32_t rpc_id,                                                      \
         const uint8_t* payload, int32_t len),                                                     \
        atlas::GetNativeApiProvider().SendClientRpc(                                                \
            entity_id, rpc_id,                                                                     \
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
    X(void, RegisterStruct,                                                                        \
        (const uint8_t* data, int32_t len),                                                        \
        atlas::GetNativeApiProvider().RegisterStruct(                                               \
            reinterpret_cast<const std::byte*>(data), len))                                        \
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
    /* ---- Script-initiated entity creation (BaseApp) ---------------------- */                   \
    /* Synchronously creates a new base entity of the given type on THIS    */                    \
    /* BaseApp: allocates an EntityID, instantiates the C# script instance  */                    \
    /* via RestoreEntity (empty blob = defaults), and — if the type has a   */                    \
    /* cell side — sends CreateCellEntity to a CellApp with the given       */                    \
    /* space_id. Returns the new entity_id, or 0 on failure (unknown type,  */                    \
    /* EntityID exhausted, …). space_id is ignored for base-only types.    */                    \
    /* Witness attachment happens later via the client-bind path; scripts   */                    \
    /* that want a non-default AoI radius call SetAoIRadius after           */                    \
    /* GiveClientTo.                                                         */                    \
    X(uint32_t, CreateBaseEntity,                                                                  \
        (uint16_t type_id, uint32_t space_id),                                                     \
        return atlas::GetNativeApiProvider().CreateBaseEntity(type_id, space_id))                  \
                                                                                                   \
    /* ---- Runtime AoI radius adjustment (BaseApp) ------------------------ */                   \
    /* Forwards cellapp::SetAoIRadius to the cell hosting this entity's     */                    \
    /* counterpart. Radius is clamped on the cell side to [0.1, max];       */                    \
    /* hysteresis is the leave-band width. Logs a warning (no-op) if the    */                    \
    /* entity has no cell counterpart.                                      */                    \
    X(void, SetAoIRadius,                                                                          \
        (uint32_t entity_id, float radius, float hysteresis),                                      \
        atlas::GetNativeApiProvider().SetAoIRadius(entity_id, radius, hysteresis))                 \
                                                                                                   \
    /* ---- C# → C++ callback table ---------------------------------------- */                   \
    X(void, SetNativeCallbacks,                                                                    \
        (const void* native_callbacks, int32_t len),                                               \
        atlas::GetNativeApiProvider().SetNativeCallbacks(native_callbacks, len))                    \
                                                                                                   \
    /* ---- CellApp spatial/replication ---------------------------------- */                    \
    /* C# tells the cell layer about the entity's new world position. C++ */                     \
    /* side updates CellEntity::position_ + range_node_ + marks the C#     */                     \
    /* volatile-dirty bit so the next BuildAndConsumeReplicationFrame      */                     \
    /* advances VolatileSeq.                                                */                     \
    X(void, SetEntityPosition,                                                                     \
        (uint32_t entity_id, float x, float y, float z),                                          \
        atlas::GetNativeApiProvider().SetEntityPosition(entity_id, x, y, z))                       \
                                                                                                   \
    /* C# hands the cell layer one tick of ReplicationFrame output. The    */                     \
    /* owner/other snapshots are used whenever event_seq > 0; otherwise    */                     \
    /* the caller passes zero-length spans.                                */                     \
    X(void, PublishReplicationFrame,                                                               \
        (uint32_t entity_id, uint64_t event_seq, uint64_t volatile_seq,                          \
         const uint8_t* owner_snap, int32_t owner_snap_len,                                        \
         const uint8_t* other_snap, int32_t other_snap_len,                                        \
         const uint8_t* owner_delta, int32_t owner_delta_len,                                      \
         const uint8_t* other_delta, int32_t other_delta_len),                                     \
        atlas::GetNativeApiProvider().PublishReplicationFrame(                                    \
            entity_id, event_seq, volatile_seq,                                                    \
            reinterpret_cast<const std::byte*>(owner_snap), owner_snap_len,                        \
            reinterpret_cast<const std::byte*>(other_snap), other_snap_len,                        \
            reinterpret_cast<const std::byte*>(owner_delta), owner_delta_len,                      \
            reinterpret_cast<const std::byte*>(other_delta), other_delta_len))                     \
                                                                                                   \
    /* ---- CellApp controllers ------------------------------------------ */                    \
    X(int32_t, AddMoveController,                                                                  \
        (uint32_t entity_id, float dest_x, float dest_y, float dest_z,                            \
         float speed, int32_t user_arg),                                                           \
        return atlas::GetNativeApiProvider().AddMoveController(                                   \
            entity_id, dest_x, dest_y, dest_z, speed, user_arg))                                   \
    X(int32_t, AddTimerController,                                                                 \
        (uint32_t entity_id, float interval, uint8_t repeat, int32_t user_arg),                  \
        return atlas::GetNativeApiProvider().AddTimerController(                                  \
            entity_id, interval, repeat != 0, user_arg))                                           \
    X(int32_t, AddProximityController,                                                             \
        (uint32_t entity_id, float range, int32_t user_arg),                                      \
        return atlas::GetNativeApiProvider().AddProximityController(                              \
            entity_id, range, user_arg))                                                           \
    X(void, CancelController,                                                                      \
        (uint32_t entity_id, int32_t controller_id),                                              \
        atlas::GetNativeApiProvider().CancelController(entity_id, controller_id))                  \
                                                                                                   \
    /* ---- Client-side telemetry --------------------------------------- */                      \
    /* Client reports an observed reliable-delta gap. Only the client    */                       \
    /* process implements this; server-side providers log+no-op.         */                       \
    X(void, ReportClientEventSeqGap,                                                               \
        (uint32_t entity_id, uint32_t gap_delta),                                                 \
        atlas::GetNativeApiProvider().ReportClientEventSeqGap(entity_id, gap_delta))
// clang-format on

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_
