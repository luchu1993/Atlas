#ifndef ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_

// Single source of truth for provider-backed Atlas* exports.
// AtlasGetAbiVersion is separate because it returns a compile-time constant.

// clang-format off
#define ATLAS_NATIVE_API_TABLE(X) \
    X(void, LogMessage, \
        (int32_t level, const char* msg, int32_t len), \
        atlas::GetNativeApiProvider().LogMessage(level, msg, len)) \
 \
    X(double, ServerTime, \
        (), \
        return atlas::GetNativeApiProvider().ServerTime()) \
    X(float, DeltaTime, \
        (), \
        return atlas::GetNativeApiProvider().DeltaTime()) \
 \
    X(uint8_t, GetProcessPrefix, \
        (), \
        return atlas::GetNativeApiProvider().GetProcessPrefix()) \
    X(void, ReportScriptTick, \
        (uint32_t entity_id, uint64_t elapsed_us), \
        atlas::GetNativeApiProvider().ReportScriptTick(entity_id, elapsed_us)) \
 \
    X(void, SendClientRpc, \
        (uint32_t entity_id, uint32_t rpc_id, uint8_t target, \
         const uint8_t* payload, int32_t len, uint64_t trace_id), \
        atlas::GetNativeApiProvider().SendClientRpc( \
            entity_id, rpc_id, static_cast<atlas::RpcTarget>(target), \
            reinterpret_cast<const std::byte*>(payload), len, trace_id)) \
    X(void, SendCellRpc, \
        (uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len, \
         uint64_t trace_id), \
        atlas::GetNativeApiProvider().SendCellRpc( \
            entity_id, rpc_id, \
            reinterpret_cast<const std::byte*>(payload), len, trace_id)) \
    X(void, SendBaseRpc, \
        (uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len, \
         uint64_t trace_id), \
        atlas::GetNativeApiProvider().SendBaseRpc( \
            entity_id, rpc_id, \
            reinterpret_cast<const std::byte*>(payload), len, trace_id)) \
    X(void, SendMovementInput, \
        (uint32_t target_entity_id, const uint8_t* frames, int32_t frame_count), \
        atlas::GetNativeApiProvider().SendMovementInput( \
            target_entity_id, reinterpret_cast<const std::byte*>(frames), frame_count)) \
    X(void, SendMovementCorrectionReport, \
        (uint32_t target_entity_id, uint32_t acked_input_seq, uint32_t server_tick, \
         float distance_m, uint16_t correction_flags), \
        atlas::GetNativeApiProvider().SendMovementCorrectionReport( \
            target_entity_id, acked_input_seq, server_tick, distance_m, correction_flags)) \
 \
    X(void, RegisterEntityType, \
        (const uint8_t* data, int32_t len), \
        atlas::GetNativeApiProvider().RegisterEntityType( \
            reinterpret_cast<const std::byte*>(data), len)) \
    X(void, UnregisterAllEntityTypes, \
        (), \
        atlas::GetNativeApiProvider().UnregisterAllEntityTypes()) \
    X(void, RegisterStruct, \
        (const uint8_t* data, int32_t len), \
        atlas::GetNativeApiProvider().RegisterStruct( \
            reinterpret_cast<const std::byte*>(data), len)) \
    X(void, RegisterComponent, \
        (const uint8_t* data, int32_t len), \
        atlas::GetNativeApiProvider().RegisterComponent( \
            reinterpret_cast<const std::byte*>(data), len)) \
    X(void, SetEntityDefDigest, \
        (const uint8_t* data, int32_t len), \
        atlas::GetNativeApiProvider().SetEntityDefDigest( \
            reinterpret_cast<const std::byte*>(data), len)) \
 \
    X(void, WriteToDb, \
        (uint32_t entity_id, const uint8_t* entity_data, int32_t len), \
        atlas::GetNativeApiProvider().WriteToDb( \
            entity_id, reinterpret_cast<const std::byte*>(entity_data), len)) \
 \
    X(void, GiveClientTo, \
        (uint32_t src_entity_id, uint32_t dest_entity_id), \
        atlas::GetNativeApiProvider().GiveClientTo(src_entity_id, dest_entity_id)) \
 \
    X(void, SetSpaceMasterType, \
        (uint32_t space_id, const char* name, int32_t len), \
        atlas::GetNativeApiProvider().SetSpaceMasterType(space_id, name, len)) \
 \
    X(uint32_t, CreateBaseEntity, \
        (uint16_t type_id, uint32_t space_id), \
        return atlas::GetNativeApiProvider().CreateBaseEntity(type_id, space_id)) \
 \
    X(uint32_t, CreateLocalCellEntity, \
        (uint16_t type_id, uint32_t space_id, float pos_x, float pos_y, float pos_z, \
         float dir_x, float dir_y, float dir_z, uint8_t on_ground), \
        return atlas::GetNativeApiProvider().CreateLocalCellEntity( \
            type_id, space_id, pos_x, pos_y, pos_z, dir_x, dir_y, dir_z, on_ground != 0)) \
    X(void, DestroyCellEntity, \
        (uint32_t entity_id), \
        atlas::GetNativeApiProvider().DestroyCellEntity(entity_id)) \
    X(uint8_t, TeleportEntity, \
        (uint32_t entity_id, uint32_t target_space_id, float pos_x, float pos_y, float pos_z, \
         float dir_x, float dir_y, float dir_z), \
        return atlas::GetNativeApiProvider().TeleportEntity( \
            entity_id, target_space_id, pos_x, pos_y, pos_z, dir_x, dir_y, dir_z) \
            ? 1 \
            : 0) \
    X(uint8_t, RequestSpawnCellOnly, \
        (uint16_t type_id, uint32_t space_id, float pos_x, float pos_y, float pos_z, \
         float dir_x, float dir_y, float dir_z, uint8_t on_ground), \
        return atlas::GetNativeApiProvider().RequestSpawnCellOnly( \
            type_id, space_id, pos_x, pos_y, pos_z, dir_x, dir_y, dir_z, on_ground != 0) \
            ? 1 \
            : 0) \
 \
    X(void, SetAoIRadius, \
        (uint32_t entity_id, float radius, float hysteresis), \
        atlas::GetNativeApiProvider().SetAoIRadius(entity_id, radius, hysteresis)) \
 \
    X(void, SetSpaceData, \
        (uint32_t space_id, uint16_t key_id, const uint8_t* value, int32_t len), \
        atlas::GetNativeApiProvider().SetSpaceData( \
            space_id, key_id, reinterpret_cast<const std::byte*>(value), len)) \
    X(void, RemoveSpaceData, \
        (uint32_t space_id, uint16_t key_id), \
        atlas::GetNativeApiProvider().RemoveSpaceData(space_id, key_id)) \
    X(uint8_t, LoadCollisionAsset, \
        (uint32_t space_id, const char* path, int32_t len), \
        return atlas::GetNativeApiProvider().LoadCollisionAsset(space_id, path, len) \
            ? 1 \
            : 0) \
    X(uint8_t, LoadNavMesh, \
        (uint32_t space_id, const char* collision_path, int32_t collision_len, \
         const char* params_path, int32_t params_len), \
        return atlas::GetNativeApiProvider().LoadNavMesh(space_id, collision_path, \
                                                         collision_len, params_path, params_len) \
            ? 1 \
            : 0) \
    X(uint32_t, GetEntitySpaceId, \
        (uint32_t entity_id), \
        return atlas::GetNativeApiProvider().GetEntitySpaceId(entity_id)) \
 \
    X(void, SetNativeCallbacks, \
        (const void* native_callbacks, int32_t len), \
        atlas::GetNativeApiProvider().SetNativeCallbacks(native_callbacks, len)) \
 \
    X(void, SetEntityPosition, \
        (uint32_t entity_id, float x, float y, float z), \
        atlas::GetNativeApiProvider().SetEntityPosition(entity_id, x, y, z)) \
    X(void, SetEntityDirection, \
        (uint32_t entity_id, float x, float y, float z), \
        atlas::GetNativeApiProvider().SetEntityDirection(entity_id, x, y, z)) \
    X(void, SetEntityOnGround, \
        (uint32_t entity_id, uint8_t on_ground), \
        atlas::GetNativeApiProvider().SetEntityOnGround(entity_id, on_ground != 0)) \
    X(void, SetMovementIntent, \
        (uint32_t entity_id, float dir_x, float dir_z, float speed_mps, uint16_t buttons), \
        atlas::GetNativeApiProvider().SetMovementIntent(entity_id, dir_x, dir_z, speed_mps, \
                                                        buttons)) \
    X(uint8_t, SetMovementCommand, \
        (uint32_t entity_id, const void* command), \
        if (command == nullptr) { \
          return 0; \
        } \
        return atlas::GetNativeApiProvider().SetMovementCommand( \
            entity_id, *static_cast<const atlas::NativeMovementCommand*>(command)) \
            ? 1 \
            : 0) \
    X(uint8_t, ClearMovementCommand, \
        (uint32_t entity_id, uint32_t command_id), \
        return atlas::GetNativeApiProvider().ClearMovementCommand(entity_id, command_id) \
            ? 1 \
            : 0) \
    X(uint8_t, SetMovementCurve, \
        (uint16_t curve_id, const float* samples, int32_t sample_count), \
        atlas::NativeMovementCurve curve; \
        curve.curve_id = curve_id; \
        curve.samples = samples; \
        curve.sample_count = sample_count; \
        return atlas::GetNativeApiProvider().SetMovementCurve(curve) ? 1 : 0) \
    X(void, GetEntityPosition, \
        (uint32_t entity_id, float* out_x, float* out_y, float* out_z), \
        atlas::GetNativeApiProvider().GetEntityPosition(entity_id, *out_x, *out_y, *out_z)) \
    X(void, GetEntityDirection, \
        (uint32_t entity_id, float* out_x, float* out_y, float* out_z), \
        atlas::GetNativeApiProvider().GetEntityDirection(entity_id, *out_x, *out_y, *out_z)) \
    X(uint8_t, GetEntityOnGround, \
        (uint32_t entity_id), \
        return atlas::GetNativeApiProvider().GetEntityOnGround(entity_id) ? 1 : 0) \
    X(uint8_t, TryGetMovementHistorySample, \
        (uint32_t entity_id, uint32_t server_tick, uint32_t* out_server_tick, \
         float* out_px, float* out_py, float* out_pz, \
         float* out_vx, float* out_vy, float* out_vz, \
         float* out_dx, float* out_dy, float* out_dz, \
         uint32_t* out_flags, uint32_t* out_last_seq), \
        atlas::NativeMovementHistorySample sample; \
        if (!atlas::GetNativeApiProvider().TryGetMovementHistorySample( \
                entity_id, server_tick, sample)) { \
          return 0; \
        } \
        *out_server_tick = sample.server_tick; \
        *out_px = sample.position_x; \
        *out_py = sample.position_y; \
        *out_pz = sample.position_z; \
        *out_vx = sample.velocity_x; \
        *out_vy = sample.velocity_y; \
        *out_vz = sample.velocity_z; \
        *out_dx = sample.direction_x; \
        *out_dy = sample.direction_y; \
        *out_dz = sample.direction_z; \
        *out_flags = sample.flags; \
        *out_last_seq = sample.last_processed_input_seq; \
        return 1) \
 \
    X(void, PublishReplicationFrame, \
        (uint32_t entity_id, uint8_t has_event, uint8_t has_volatile, \
         const uint8_t* owner_snap, int32_t owner_snap_len, \
         const uint8_t* other_snap, int32_t other_snap_len, \
         const uint8_t* owner_delta, int32_t owner_delta_len, \
         const uint8_t* other_delta, int32_t other_delta_len), \
        atlas::GetNativeApiProvider().PublishReplicationFrame( \
            entity_id, has_event != 0, has_volatile != 0, \
            reinterpret_cast<const std::byte*>(owner_snap), owner_snap_len, \
            reinterpret_cast<const std::byte*>(other_snap), other_snap_len, \
            reinterpret_cast<const std::byte*>(owner_delta), owner_delta_len, \
            reinterpret_cast<const std::byte*>(other_delta), other_delta_len)) \
 \
    X(int32_t, AddMoveController, \
        (uint32_t entity_id, float dest_x, float dest_y, float dest_z, \
         float speed, int32_t user_arg), \
        return atlas::GetNativeApiProvider().AddMoveController( \
            entity_id, dest_x, dest_y, dest_z, speed, user_arg)) \
    X(int32_t, AddTimerController, \
        (uint32_t entity_id, float interval, uint8_t repeat, int32_t user_arg), \
        return atlas::GetNativeApiProvider().AddTimerController( \
            entity_id, interval, repeat != 0, user_arg)) \
    X(int32_t, AddProximityController, \
        (uint32_t entity_id, float range, int32_t user_arg), \
        return atlas::GetNativeApiProvider().AddProximityController( \
            entity_id, range, user_arg)) \
    X(void, CancelController, \
        (uint32_t entity_id, int32_t controller_id), \
        atlas::GetNativeApiProvider().CancelController(entity_id, controller_id)) \
 \
    X(void, ReportClientEventSeqGap, \
        (uint32_t entity_id, uint32_t gap_delta), \
        atlas::GetNativeApiProvider().ReportClientEventSeqGap(entity_id, gap_delta)) \
 \
    X(uint64_t, CoroRegisterPending, \
        (uint16_t reply_id, uint32_t request_id, int32_t timeout_ms, intptr_t managed_handle), \
        return atlas::GetNativeApiProvider().CoroRegisterPending( \
            reply_id, request_id, timeout_ms, managed_handle)) \
    X(void, CoroCancelPending, \
        (uint64_t handle), \
        atlas::GetNativeApiProvider().CoroCancelPending(handle)) \
 \
    X(void, SendEntityRpcSuccess, \
        (intptr_t reply_channel, uint32_t request_id, const std::byte* body, int32_t len), \
        atlas::GetNativeApiProvider().SendEntityRpcSuccess( \
            reply_channel, request_id, body, len)) \
    X(void, SendEntityRpcFailure, \
        (intptr_t reply_channel, uint32_t request_id, int32_t error_code, \
         const char* msg, int32_t msg_len), \
        atlas::GetNativeApiProvider().SendEntityRpcFailure( \
            reply_channel, request_id, error_code, msg, msg_len))
// clang-format on

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_DEFS_H_
