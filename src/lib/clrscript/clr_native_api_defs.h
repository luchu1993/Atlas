#ifndef ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_

// Single source of truth for provider-backed Atlas* exports.
// AtlasGetAbiVersion is separate because it returns a compile-time constant.

// clang-format off
#define ATLAS_NATIVE_API_TABLE(X)                                                                  \
    X(void, LogMessage,                                                                            \
        (int32_t level, const char* msg, int32_t len),                                             \
        atlas::GetNativeApiProvider().LogMessage(level, msg, len))                                  \
                                                                                                   \
    X(double, ServerTime,                                                                          \
        (),                                                                                        \
        return atlas::GetNativeApiProvider().ServerTime())                                          \
    X(float, DeltaTime,                                                                            \
        (),                                                                                        \
        return atlas::GetNativeApiProvider().DeltaTime())                                           \
                                                                                                   \
    X(uint8_t, GetProcessPrefix,                                                                   \
        (),                                                                                        \
        return atlas::GetNativeApiProvider().GetProcessPrefix())                                    \
                                                                                                   \
    X(void, SendClientRpc,                                                                         \
        (uint32_t entity_id, uint32_t rpc_id, uint8_t target,                                      \
         const uint8_t* payload, int32_t len),                                                     \
        atlas::GetNativeApiProvider().SendClientRpc(                                                \
            entity_id, rpc_id, static_cast<atlas::RpcTarget>(target),                              \
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
    X(void, WriteToDb,                                                                             \
        (uint32_t entity_id, const uint8_t* entity_data, int32_t len),                            \
        atlas::GetNativeApiProvider().WriteToDb(                                                    \
            entity_id, reinterpret_cast<const std::byte*>(entity_data), len))                     \
                                                                                                   \
    X(void, GiveClientTo,                                                                          \
        (uint32_t src_entity_id, uint32_t dest_entity_id),                                        \
        atlas::GetNativeApiProvider().GiveClientTo(src_entity_id, dest_entity_id))                  \
                                                                                                   \
    X(uint32_t, CreateBaseEntity,                                                                  \
        (uint16_t type_id, uint32_t space_id),                                                     \
        return atlas::GetNativeApiProvider().CreateBaseEntity(type_id, space_id))                  \
                                                                                                   \
    X(void, SetAoIRadius,                                                                          \
        (uint32_t entity_id, float radius, float hysteresis),                                      \
        atlas::GetNativeApiProvider().SetAoIRadius(entity_id, radius, hysteresis))                 \
                                                                                                   \
    X(void, SetNativeCallbacks,                                                                    \
        (const void* native_callbacks, int32_t len),                                               \
        atlas::GetNativeApiProvider().SetNativeCallbacks(native_callbacks, len))                    \
                                                                                                   \
    X(void, SetEntityPosition,                                                                     \
        (uint32_t entity_id, float x, float y, float z),                                          \
        atlas::GetNativeApiProvider().SetEntityPosition(entity_id, x, y, z))                       \
                                                                                                   \
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
    X(void, ReportClientEventSeqGap,                                                               \
        (uint32_t entity_id, uint32_t gap_delta),                                                 \
        atlas::GetNativeApiProvider().ReportClientEventSeqGap(entity_id, gap_delta))
// clang-format on

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_
