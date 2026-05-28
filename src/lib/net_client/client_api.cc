#include "net_client/client_api.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "foundation/log.h"
#include "movement_sim/movement_sim.h"
#include "net_client/client_session.h"

namespace {

thread_local std::string g_global_last_error;
std::atomic<AtlasLogFn> g_log_handler{nullptr};

class CallbackLogSink final : public atlas::LogSink {
 public:
  void Write(atlas::LogLevel level, std::string_view /*category*/, std::string_view message,
             const std::source_location& /*location*/) override {
    AtlasLogFn fn = g_log_handler.load(std::memory_order_acquire);
    if (!fn) return;
    fn(static_cast<int32_t>(level), message.data(), static_cast<int32_t>(message.size()));
  }
  void Flush() override {}
};

void EnsureLogSinkInstalled() {
  static std::once_flag once;
  std::call_once(once,
                 [] { atlas::Logger::Instance().AddSink(std::make_shared<CallbackLogSink>()); });
}

auto AbiCompatible(uint32_t expected) -> bool {
  constexpr uint32_t kOur = ATLAS_NET_ABI_VERSION;
  const uint32_t our_major = (kOur >> 24) & 0xFFu;
  const uint32_t our_minor = (kOur >> 16) & 0xFFu;
  const uint32_t exp_major = (expected >> 24) & 0xFFu;
  const uint32_t exp_minor = (expected >> 16) & 0xFFu;
  return exp_major == our_major && exp_minor <= our_minor;
}

auto ToMovementInput(const AtlasMovementInputFrame& frame) -> atlas::movement::InputFrame {
  atlas::movement::InputFrame input;
  input.seq = frame.seq;
  input.input_tick = frame.input_tick;
  input.move_x = frame.move_x;
  input.move_z = frame.move_z;
  input.view_yaw = frame.view_yaw;
  input.view_pitch = frame.view_pitch;
  input.buttons = frame.buttons;
  input.client_dt_ms = frame.client_dt_ms;
  return input;
}

auto ToMovementState(const AtlasMovementStateFrame& frame) -> atlas::movement::MovementState {
  atlas::movement::MovementState state;
  state.position = {frame.position_x, frame.position_y, frame.position_z};
  state.velocity = {frame.velocity_x, frame.velocity_y, frame.velocity_z};
  state.direction = {frame.direction_x, frame.direction_y, frame.direction_z};
  state.flags = frame.flags;
  state.last_processed_input_seq = frame.last_processed_input_seq;
  return state;
}

auto ToApiState(const atlas::movement::MovementState& state) -> AtlasMovementStateFrame {
  AtlasMovementStateFrame frame;
  frame.position_x = state.position.x;
  frame.position_y = state.position.y;
  frame.position_z = state.position.z;
  frame.velocity_x = state.velocity.x;
  frame.velocity_y = state.velocity.y;
  frame.velocity_z = state.velocity.z;
  frame.direction_x = state.direction.x;
  frame.direction_y = state.direction.y;
  frame.direction_z = state.direction.z;
  frame.flags = state.flags;
  frame.last_processed_input_seq = state.last_processed_input_seq;
  return frame;
}

auto ToApiCorrectionTier(
    atlas::movement::CorrectionTier tier) -> AtlasMovementCorrectionTier {
  switch (tier) {
    case atlas::movement::CorrectionTier::kNone:
      return ATLAS_MOVEMENT_CORRECTION_NONE;
    case atlas::movement::CorrectionTier::kTier1:
      return ATLAS_MOVEMENT_CORRECTION_TIER1;
    case atlas::movement::CorrectionTier::kTier2:
      return ATLAS_MOVEMENT_CORRECTION_TIER2;
    case atlas::movement::CorrectionTier::kSnap:
      return ATLAS_MOVEMENT_CORRECTION_SNAP;
  }
  return ATLAS_MOVEMENT_CORRECTION_NONE;
}

}  // namespace

extern "C" {

uint32_t AtlasNetGetAbiVersion(void) {
  return ATLAS_NET_ABI_VERSION;
}

const char* AtlasNetLastError(AtlasNetContext* ctx) {
  if (!ctx) return AtlasNetGlobalLastError();
  const auto& err = ctx->LastError();
  return err.empty() ? "" : err.c_str();
}

const char* AtlasNetGlobalLastError(void) {
  return g_global_last_error.empty() ? "" : g_global_last_error.c_str();
}

AtlasNetContext* AtlasNetCreate(uint32_t expected_abi) {
  if (!AbiCompatible(expected_abi)) {
    g_global_last_error = "ABI version mismatch";
    ATLAS_LOG_ERROR("AtlasNetCreate: ABI mismatch (caller=0x{:08x}, dll=0x{:08x})", expected_abi,
                    static_cast<uint32_t>(ATLAS_NET_ABI_VERSION));
    return nullptr;
  }

  AtlasNetContext* ctx = nullptr;
  try {
    ctx = new AtlasNetContext{};
  } catch (const std::bad_alloc&) {
    g_global_last_error = "ctx allocation failed";
    return nullptr;
  }

  AtlasNetCallbacks empty_table{};
  (void)ctx->SetCallbacks(empty_table);
  return ctx;
}

void AtlasNetDestroy(AtlasNetContext* ctx) {
  if (!ctx) return;
  ctx->Disconnect(ATLAS_DISCONNECT_INTERNAL);
  delete ctx;
}

int32_t AtlasNetPoll(AtlasNetContext* ctx) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->Poll();
}

AtlasNetState AtlasNetGetState(AtlasNetContext* ctx) {
  if (!ctx) return ATLAS_NET_STATE_DISCONNECTED;
  return ctx->GetState();
}

int32_t AtlasNetLogin(AtlasNetContext* ctx, const char* loginapp_host, uint16_t loginapp_port,
                      const char* username, const char* password_hash, AtlasLoginResultFn callback,
                      void* user_data) {
  if (!ctx || !loginapp_host || !username || !password_hash) {
    return ATLAS_NET_ERR_INVAL;
  }
  return ctx->StartLogin(loginapp_host, loginapp_port, username, password_hash, callback,
                         user_data);
}

int32_t AtlasNetAuthenticate(AtlasNetContext* ctx, AtlasAuthResultFn callback, void* user_data) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->StartAuthenticate(callback, user_data);
}

int32_t AtlasNetSetEntityDefDigest(AtlasNetContext* ctx, const uint8_t* data, int32_t len) {
  if (!ctx || !data || len != 32) return ATLAS_NET_ERR_INVAL;
  ctx->SetEntityDefDigest(data, len);
  return ATLAS_NET_OK;
}

int32_t AtlasNetDisconnect(AtlasNetContext* ctx, AtlasDisconnectReason reason) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->Disconnect(reason);
}

int32_t AtlasNetSetTransportImpairment(AtlasNetContext* ctx, uint32_t one_way_latency_ms,
                                       uint32_t loss_permyriad, uint32_t seed) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->SetTransportImpairment(one_way_latency_ms, loss_permyriad, seed);
}

int32_t AtlasNetSendBaseRpc(AtlasNetContext* ctx, uint32_t entity_id, uint32_t rpc_id,
                            const uint8_t* payload, int32_t len) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->SendBaseRpc(entity_id, rpc_id, payload, len);
}

int32_t AtlasNetSendCellRpc(AtlasNetContext* ctx, uint32_t entity_id, uint32_t rpc_id,
                            const uint8_t* payload, int32_t len) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->SendCellRpc(entity_id, rpc_id, payload, len);
}

int32_t AtlasNetSendMovementInput(AtlasNetContext* ctx, uint32_t target_entity_id,
                                  const AtlasMovementInputFrame* frames, int32_t frame_count) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->SendMovementInput(target_entity_id, frames, frame_count);
}

int32_t AtlasNetSendMovementCorrectionReport(AtlasNetContext* ctx, uint32_t target_entity_id,
                                             uint32_t acked_input_seq, uint32_t server_tick,
                                             float distance_m, uint16_t correction_flags) {
  if (!ctx) return ATLAS_NET_ERR_INVAL;
  return ctx->SendMovementCorrectionReport(target_entity_id, acked_input_seq, server_tick,
                                           distance_m, correction_flags);
}

int32_t AtlasNetMovementPredictStep(const AtlasMovementStateFrame* previous,
                                    const AtlasMovementInputFrame* input, uint32_t server_tick,
                                    AtlasMovementStateFrame* out_state) {
  if (!previous || !input || !out_state) return ATLAS_NET_ERR_INVAL;

  const auto previous_state = ToMovementState(*previous);
  if (!atlas::movement::IsFinite(previous_state)) return ATLAS_NET_ERR_INVAL;
  const auto movement_input = ToMovementInput(*input);
  if (!atlas::movement::IsInputFrameValid(movement_input)) return ATLAS_NET_ERR_INVAL;

  atlas::movement::MovementConfig config;
  if (!atlas::movement::IsStateWithinLimits(previous_state, config)) return ATLAS_NET_ERR_INVAL;
  atlas::movement::FlatGroundQuery query;
  const auto result =
      atlas::movement::Step(previous_state, movement_input, config, query, server_tick);
  if (!atlas::movement::IsStateWithinLimits(result.state, config)) return ATLAS_NET_ERR_INVAL;

  *out_state = ToApiState(result.state);
  return ATLAS_NET_OK;
}

AtlasMovementCorrectionTier AtlasNetMovementClassifyCorrection(float distance_m) {
  return ToApiCorrectionTier(atlas::movement::ClassifyCorrection(distance_m));
}

uint16_t AtlasNetMovementCorrectionFlag(float distance_m) {
  return atlas::movement::CorrectionFlagForTier(
      atlas::movement::ClassifyCorrection(distance_m));
}

int32_t AtlasNetSetCallbacks(AtlasNetContext* ctx, const AtlasNetCallbacks* callbacks) {
  if (!ctx || !callbacks) return ATLAS_NET_ERR_INVAL;
  return ctx->SetCallbacks(*callbacks);
}

void AtlasNetSetLogHandler(AtlasLogFn handler) {
  g_log_handler.store(handler, std::memory_order_release);
  if (handler) EnsureLogSinkInstalled();
}

int32_t AtlasNetGetStats(AtlasNetContext* ctx, AtlasNetStats* out_stats) {
  if (!ctx || !out_stats) return ATLAS_NET_ERR_INVAL;
  return ctx->FillStats(out_stats);
}

}  // extern "C"
