#ifndef ATLAS_LIB_NET_CLIENT_CLIENT_API_H_
#define ATLAS_LIB_NET_CLIENT_CLIENT_API_H_

// Public C API — full contract: docs/client/UNITY_NATIVE_DLL_DESIGN.md §4.

#include <stdint.h>

#include "net_client/net_client_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// ABI layout: [MAJOR:8][MINOR:8][PATCH:16]; Create rejects major mismatch or newer caller minor.
// 0x02050000 adds transport impairment hooks for movement prediction validation.
#define ATLAS_NET_ABI_VERSION 0x02050000u

#define ATLAS_NET_OK 0
#define ATLAS_NET_ERR_BUSY -16
#define ATLAS_NET_ERR_NOMEM -12
#define ATLAS_NET_ERR_INVAL -22
#define ATLAS_NET_ERR_NOCONN -107
#define ATLAS_NET_ERR_ABI -1000

// Ctx is single-thread-owned (Unity main thread).
typedef struct AtlasNetContext AtlasNetContext;

ATLAS_NET_API uint32_t AtlasNetGetAbiVersion(void);

// View valid until the next API call on the same ctx.
ATLAS_NET_API const char* AtlasNetLastError(AtlasNetContext* ctx);

// Thread-local; only meaningful immediately after AtlasNetCreate failure.
ATLAS_NET_API const char* AtlasNetGlobalLastError(void);

// Returns NULL on ABI mismatch / OOM. Pre-installs noop callback table.
ATLAS_NET_API AtlasNetContext* AtlasNetCreate(uint32_t expected_abi);

// Tears down channels, zeroes SessionKey. NULL is a noop.
ATLAS_NET_API void AtlasNetDestroy(AtlasNetContext* ctx);

// Single tick of the underlying dispatcher; callbacks fire synchronously here.
ATLAS_NET_API int32_t AtlasNetPoll(AtlasNetContext* ctx);

typedef enum AtlasNetState {
  ATLAS_NET_STATE_DISCONNECTED = 0,
  ATLAS_NET_STATE_LOGGING_IN = 1,
  ATLAS_NET_STATE_LOGIN_SUCCEEDED = 2,
  ATLAS_NET_STATE_AUTHENTICATING = 3,
  ATLAS_NET_STATE_CONNECTED = 4,
} AtlasNetState;

ATLAS_NET_API AtlasNetState AtlasNetGetState(AtlasNetContext* ctx);

typedef enum AtlasLoginStatus {
  ATLAS_LOGIN_SUCCESS = 0,
  ATLAS_LOGIN_INVALID_CREDENTIALS = 1,
  ATLAS_LOGIN_ALREADY_LOGGED_IN = 2,
  ATLAS_LOGIN_SERVER_FULL = 3,
  ATLAS_LOGIN_TIMEOUT = 4,
  ATLAS_LOGIN_NETWORK_ERROR = 5,
  // Server's entity-def SHA-256 disagrees with what the client stamped onto
  // LoginRequest; client must update its build before reconnecting.
  ATLAS_LOGIN_DEF_MISMATCH = 6,
  ATLAS_LOGIN_INTERNAL_ERROR = 255,
} AtlasLoginStatus;

// Strings are non-owning views, valid only inside the callback body.
typedef void (*AtlasLoginResultFn)(void* user_data, uint8_t status, const char* baseapp_host,
                                   uint16_t baseapp_port, const char* error_message);

typedef void (*AtlasAuthResultFn)(void* user_data, uint8_t success, uint32_t entity_id,
                                  uint16_t type_id, const char* error_message);

// Legal only in DISCONNECTED. Result delivered from a future Poll.
ATLAS_NET_API int32_t AtlasNetLogin(AtlasNetContext* ctx, const char* loginapp_host,
                                    uint16_t loginapp_port, const char* username,
                                    const char* password_hash, AtlasLoginResultFn callback,
                                    void* user_data);

// 32-byte SHA-256 stamped onto every LoginRequest; BaseApp rejects
// mismatched .def builds. Pass Atlas.Rpc.EntityDefDigest.Bytes once.
ATLAS_NET_API int32_t AtlasNetSetEntityDefDigest(AtlasNetContext* ctx, const uint8_t* data,
                                                 int32_t len);

// SessionKey + BaseApp address come from cached state — never crosses FFI.
ATLAS_NET_API int32_t AtlasNetAuthenticate(AtlasNetContext* ctx, AtlasAuthResultFn callback,
                                           void* user_data);

typedef enum AtlasDisconnectReason {
  ATLAS_DISCONNECT_USER = 0,
  ATLAS_DISCONNECT_LOGOUT = 1,
  ATLAS_DISCONNECT_INTERNAL = 2,
} AtlasDisconnectReason;

// LOGOUT fires on_disconnect with callback reason=3; USER is silent.
ATLAS_NET_API int32_t AtlasNetDisconnect(AtlasNetContext* ctx, AtlasDisconnectReason reason);

// Test hook: applies bidirectional RUDP latency/loss to current and future
// LoginApp/BaseApp channels. loss_permyriad uses 10000 == 100%.
ATLAS_NET_API int32_t AtlasNetSetTransportImpairment(AtlasNetContext* ctx,
                                                     uint32_t one_way_latency_ms,
                                                     uint32_t loss_permyriad,
                                                     uint32_t seed);

ATLAS_NET_API int32_t AtlasNetSendBaseRpc(AtlasNetContext* ctx, uint32_t entity_id, uint32_t rpc_id,
                                          const uint8_t* payload, int32_t len);

ATLAS_NET_API int32_t AtlasNetSendCellRpc(AtlasNetContext* ctx, uint32_t entity_id, uint32_t rpc_id,
                                          const uint8_t* payload, int32_t len);

#pragma pack(push, 1)
typedef struct AtlasMovementInputFrame {
  uint32_t seq;
  uint32_t input_tick;
  int8_t move_x;
  int8_t move_z;
  uint16_t view_yaw;
  int8_t view_pitch;
  uint16_t buttons;
  uint16_t client_dt_ms;
} AtlasMovementInputFrame;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct AtlasMovementStateFrame {
  float position_x;
  float position_y;
  float position_z;
  float velocity_x;
  float velocity_y;
  float velocity_z;
  float direction_x;
  float direction_y;
  float direction_z;
  uint32_t flags;
  uint32_t last_processed_input_seq;
} AtlasMovementStateFrame;
#pragma pack(pop)

typedef enum AtlasMovementCorrectionTier {
  ATLAS_MOVEMENT_CORRECTION_NONE = 0,
  ATLAS_MOVEMENT_CORRECTION_TIER1 = 1,
  ATLAS_MOVEMENT_CORRECTION_TIER2 = 2,
  ATLAS_MOVEMENT_CORRECTION_SNAP = 3,
} AtlasMovementCorrectionTier;

ATLAS_NET_API int32_t AtlasNetSendMovementInput(AtlasNetContext* ctx, uint32_t target_entity_id,
                                                const AtlasMovementInputFrame* frames,
                                                int32_t frame_count);

ATLAS_NET_API int32_t AtlasNetSendMovementCorrectionReport(
    AtlasNetContext* ctx, uint32_t target_entity_id, uint32_t acked_input_seq,
    uint32_t server_tick, float distance_m, uint16_t correction_flags);

ATLAS_NET_API int32_t AtlasNetMovementPredictStep(const AtlasMovementStateFrame* previous,
                                                  const AtlasMovementInputFrame* input,
                                                  uint32_t server_tick,
                                                  AtlasMovementStateFrame* out_state);

ATLAS_NET_API AtlasMovementCorrectionTier AtlasNetMovementClassifyCorrection(float distance_m);
ATLAS_NET_API uint16_t AtlasNetMovementCorrectionFlag(float distance_m);

// All payload pointers are views; copy before returning.
typedef void (*AtlasDisconnectFn)(AtlasNetContext* ctx, int32_t reason);

// Every server-bound message lands here after auth: raw wire id 0xF001..0xF007;
// login/auth use dedicated callbacks. Payload is valid only for the call.
typedef void (*AtlasDeliverFromServerFn)(AtlasNetContext* ctx, uint16_t msg_id,
                                         const uint8_t* payload, int32_t len);

// NULL fields are substituted with internal noops on SetCallbacks.
// New fields must append; layout pinned for ABI.
#pragma pack(push, 1)
typedef struct AtlasNetCallbacks {
  AtlasDisconnectFn on_disconnect;
  AtlasDeliverFromServerFn on_deliver;
} AtlasNetCallbacks;
#pragma pack(pop)

ATLAS_NET_API int32_t AtlasNetSetCallbacks(AtlasNetContext* ctx,
                                           const AtlasNetCallbacks* callbacks);

// Process-global; pass NULL to detach.
typedef void (*AtlasLogFn)(int32_t level, const char* message, int32_t len);

ATLAS_NET_API void AtlasNetSetLogHandler(AtlasLogFn handler);

#pragma pack(push, 1)
typedef struct AtlasNetStats {
  uint32_t rtt_ms;
  uint32_t bytes_sent;
  uint32_t bytes_recv;
  uint32_t packets_lost;
  uint32_t send_queue_size;
  float loss_rate;
} AtlasNetStats;
#pragma pack(pop)

ATLAS_NET_API int32_t AtlasNetGetStats(AtlasNetContext* ctx, AtlasNetStats* out_stats);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ATLAS_LIB_NET_CLIENT_CLIENT_API_H_
