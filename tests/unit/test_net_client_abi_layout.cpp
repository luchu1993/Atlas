#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "foundation/log.h"
#include "movement_sim/movement_sim.h"
#include "net_client/client_api.h"

static_assert(sizeof(AtlasNetCallbacks) == 2 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_disconnect) == 0 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_deliver) == 1 * sizeof(void*));

static_assert(sizeof(AtlasMovementInputFrame) == 17);
static_assert(offsetof(AtlasMovementInputFrame, seq) == 0);
static_assert(offsetof(AtlasMovementInputFrame, input_tick) == 4);
static_assert(offsetof(AtlasMovementInputFrame, move_x) == 8);
static_assert(offsetof(AtlasMovementInputFrame, move_z) == 9);
static_assert(offsetof(AtlasMovementInputFrame, view_yaw) == 10);
static_assert(offsetof(AtlasMovementInputFrame, view_pitch) == 12);
static_assert(offsetof(AtlasMovementInputFrame, buttons) == 13);
static_assert(offsetof(AtlasMovementInputFrame, client_dt_ms) == 15);

static_assert(sizeof(AtlasMovementStateFrame) == 44);
static_assert(offsetof(AtlasMovementStateFrame, position_x) == 0);
static_assert(offsetof(AtlasMovementStateFrame, velocity_x) == 12);
static_assert(offsetof(AtlasMovementStateFrame, direction_x) == 24);
static_assert(offsetof(AtlasMovementStateFrame, flags) == 36);
static_assert(offsetof(AtlasMovementStateFrame, last_processed_input_seq) == 40);

static_assert(sizeof(AtlasNetStats) == 24);
static_assert(offsetof(AtlasNetStats, rtt_ms) == 0);
static_assert(offsetof(AtlasNetStats, bytes_sent) == 4);
static_assert(offsetof(AtlasNetStats, bytes_recv) == 8);
static_assert(offsetof(AtlasNetStats, packets_lost) == 12);
static_assert(offsetof(AtlasNetStats, send_queue_size) == 16);
static_assert(offsetof(AtlasNetStats, loss_rate) == 20);

TEST(NetClientAbi, VersionMatchesHeaderConstant) {
  EXPECT_EQ(AtlasNetGetAbiVersion(), ATLAS_NET_ABI_VERSION);
}

TEST(NetClientAbi, CreateRejectsMajorMismatch) {
  constexpr uint32_t kBadMajor = 0x03000000u;
  auto* ctx = AtlasNetCreate(kBadMajor);
  EXPECT_EQ(ctx, nullptr);
  EXPECT_NE(AtlasNetGlobalLastError(), nullptr);
}

TEST(NetClientAbi, CreateAcceptsMatchingVersion) {
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_DISCONNECTED);
  AtlasNetDestroy(ctx);
}

TEST(NetClientAbi, SetCallbacksAcceptsAllNullSlots) {
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  AtlasNetCallbacks cbs{};
  EXPECT_EQ(AtlasNetSetCallbacks(ctx, &cbs), ATLAS_NET_OK);
  AtlasNetDestroy(ctx);
}

TEST(NetClientAbi, SetTransportImpairmentAcceptsValidRange) {
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(AtlasNetSetTransportImpairment(ctx, 75, 200, 123), ATLAS_NET_OK);
  EXPECT_EQ(AtlasNetSetTransportImpairment(ctx, 0, 0, 0), ATLAS_NET_OK);
  AtlasNetDestroy(ctx);
}

TEST(NetClientAbi, SetTransportImpairmentRejectsInvalidLoss) {
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(AtlasNetSetTransportImpairment(ctx, 0, 10'001, 1), ATLAS_NET_ERR_INVAL);
  EXPECT_NE(std::string(AtlasNetLastError(ctx)).find("loss_permyriad"), std::string::npos);
  AtlasNetDestroy(ctx);
}

TEST(NetClientAbi, NullCtxReturnsInvalArg) {
  EXPECT_EQ(AtlasNetPoll(nullptr), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetDisconnect(nullptr, ATLAS_DISCONNECT_USER), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSetTransportImpairment(nullptr, 0, 0, 1), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSendBaseRpc(nullptr, 0, 0, nullptr, 0), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSendCellRpc(nullptr, 0, 0, nullptr, 0), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSendMovementInput(nullptr, 0, nullptr, 0), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSendMovementCorrectionReport(nullptr, 0, 0, 0, 0.0f, 0),
            ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetMovementPredictStep(nullptr, nullptr, 0, nullptr), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSetCallbacks(nullptr, nullptr), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetGetStats(nullptr, nullptr), ATLAS_NET_ERR_INVAL);
}

namespace {
struct LogCapture {
  std::atomic<int> count{0};
  std::string last;
  int last_level{-1};
};
LogCapture* g_capture = nullptr;

void CaptureLog(int32_t level, const char* msg, int32_t len) {
  if (!g_capture) return;
  g_capture->count.fetch_add(1, std::memory_order_relaxed);
  g_capture->last.assign(msg, static_cast<size_t>(len));
  g_capture->last_level = level;
}

auto ToApiState(const atlas::movement::MovementState& state) -> AtlasMovementStateFrame {
  AtlasMovementStateFrame frame{};
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

auto ToApiInput(const atlas::movement::InputFrame& input) -> AtlasMovementInputFrame {
  AtlasMovementInputFrame frame{};
  frame.seq = input.seq;
  frame.input_tick = input.input_tick;
  frame.move_x = input.move_x;
  frame.move_z = input.move_z;
  frame.view_yaw = input.view_yaw;
  frame.view_pitch = input.view_pitch;
  frame.buttons = input.buttons;
  frame.client_dt_ms = input.client_dt_ms;
  return frame;
}

auto NextRandom(uint32_t& state) -> uint32_t {
  state = state * 1664525u + 1013904223u;
  return state;
}

auto QuantizedAxis(uint32_t value) -> int8_t {
  return static_cast<int8_t>(static_cast<int>((value >> 24) % 255u) - 127);
}

auto StatesClose(const AtlasMovementStateFrame& api_state,
                 const atlas::movement::MovementState& server_state) -> bool {
  constexpr float kEpsilon = 0.00001f;
  return std::fabs(api_state.position_x - server_state.position.x) <= kEpsilon &&
         std::fabs(api_state.position_y - server_state.position.y) <= kEpsilon &&
         std::fabs(api_state.position_z - server_state.position.z) <= kEpsilon &&
         std::fabs(api_state.velocity_x - server_state.velocity.x) <= kEpsilon &&
         std::fabs(api_state.velocity_y - server_state.velocity.y) <= kEpsilon &&
         std::fabs(api_state.velocity_z - server_state.velocity.z) <= kEpsilon &&
         std::fabs(api_state.direction_x - server_state.direction.x) <= kEpsilon &&
         std::fabs(api_state.direction_y - server_state.direction.y) <= kEpsilon &&
         std::fabs(api_state.direction_z - server_state.direction.z) <= kEpsilon &&
         api_state.flags == server_state.flags &&
         api_state.last_processed_input_seq == server_state.last_processed_input_seq;
}
}  // namespace

TEST(NetClientAbi, LogHandlerReceivesMessages) {
  LogCapture capture;
  g_capture = &capture;
  AtlasNetSetLogHandler(&CaptureLog);

  ATLAS_LOG_ERROR("net_client log probe {}", 42);

  AtlasNetSetLogHandler(nullptr);
  g_capture = nullptr;

  EXPECT_GE(capture.count.load(), 1);
  EXPECT_NE(capture.last.find("net_client log probe 42"), std::string::npos);
  EXPECT_EQ(capture.last_level, static_cast<int>(atlas::LogLevel::kError));
}

TEST(NetClientAbi, StateMatrixRejectsIllegalCalls) {
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);

  EXPECT_EQ(AtlasNetAuthenticate(
                ctx, [](void*, uint8_t, uint32_t, uint16_t, const char*) {}, nullptr),
            ATLAS_NET_ERR_BUSY);
  EXPECT_EQ(AtlasNetSendBaseRpc(ctx, 1, 1, nullptr, 0), ATLAS_NET_ERR_NOCONN);
  EXPECT_EQ(AtlasNetSendCellRpc(ctx, 1, 1, nullptr, 0), ATLAS_NET_ERR_NOCONN);
  AtlasMovementInputFrame frame{};
  EXPECT_EQ(AtlasNetSendMovementInput(ctx, 1, &frame, 1), ATLAS_NET_ERR_NOCONN);
  EXPECT_EQ(AtlasNetSendMovementCorrectionReport(ctx, 1, 1, 1, 0.0f, 0),
            ATLAS_NET_ERR_NOCONN);

  AtlasNetDestroy(ctx);
}

TEST(NetClientAbi, MovementPredictStepUsesSharedFlatGroundMotor) {
  AtlasMovementStateFrame state{};
  state.direction_z = 1.0f;
  state.flags = 1u;

  AtlasMovementInputFrame input{};
  input.seq = 7;
  input.input_tick = 11;
  input.move_z = 127;
  input.client_dt_ms = 33;

  AtlasMovementStateFrame predicted{};
  EXPECT_EQ(AtlasNetMovementPredictStep(&state, &input, 11, &predicted), ATLAS_NET_OK);
  EXPECT_GT(predicted.position_z, 0.0f);
  EXPECT_GT(predicted.velocity_z, 0.0f);
  EXPECT_EQ(predicted.last_processed_input_seq, 7u);
}

TEST(NetClientAbi, MovementCorrectionApiUsesSharedThresholds) {
  EXPECT_EQ(AtlasNetMovementClassifyCorrection(0.29f), ATLAS_MOVEMENT_CORRECTION_NONE);
  EXPECT_EQ(AtlasNetMovementClassifyCorrection(atlas::movement::kCorrectionTier1DistanceM),
            ATLAS_MOVEMENT_CORRECTION_TIER1);
  EXPECT_EQ(AtlasNetMovementClassifyCorrection(atlas::movement::kCorrectionTier2DistanceM),
            ATLAS_MOVEMENT_CORRECTION_TIER2);
  EXPECT_EQ(AtlasNetMovementClassifyCorrection(atlas::movement::kCorrectionSnapDistanceM),
            ATLAS_MOVEMENT_CORRECTION_SNAP);
  EXPECT_EQ(AtlasNetMovementCorrectionFlag(atlas::movement::kCorrectionSnapDistanceM),
            atlas::movement::kCorrectionFlagSnap);
}

TEST(NetClientAbi, MovementPredictStepMatchesServerMotorSequence) {
  atlas::movement::MovementState server_state;
  AtlasMovementStateFrame api_state = ToApiState(server_state);
  atlas::movement::MovementConfig config;
  atlas::movement::FlatGroundQuery query;
  uint32_t rng = 0xA71A5001u;

  for (uint32_t tick = 1; tick <= 10000; ++tick) {
    atlas::movement::InputFrame input;
    input.seq = tick;
    input.input_tick = tick;
    input.move_x = QuantizedAxis(NextRandom(rng));
    input.move_z = QuantizedAxis(NextRandom(rng));
    input.view_yaw = static_cast<uint16_t>(NextRandom(rng) & 0xFFFFu);
    input.view_pitch = static_cast<int8_t>(static_cast<int>(NextRandom(rng) % 61u) - 30);
    input.buttons = (tick % 113u) == 0u ? atlas::movement::kInputButtonJump : 0u;
    input.client_dt_ms = 33;

    const auto server_result = atlas::movement::Step(server_state, input, config, query, tick);
    const AtlasMovementInputFrame api_input = ToApiInput(input);
    AtlasMovementStateFrame predicted{};
    ASSERT_EQ(AtlasNetMovementPredictStep(&api_state, &api_input, tick, &predicted), ATLAS_NET_OK);
    if (!StatesClose(predicted, server_result.state)) {
      ADD_FAILURE() << "movement predictor diverged at tick " << tick;
      break;
    }
    server_state = server_result.state;
    api_state = predicted;
  }

  EXPECT_EQ(api_state.last_processed_input_seq, 10000u);
}

TEST(NetClientAbi, MovementPredictStepRejectsNonFiniteState) {
  AtlasMovementStateFrame state{};
  state.position_x = std::numeric_limits<float>::quiet_NaN();
  AtlasMovementInputFrame input{};
  AtlasMovementStateFrame predicted{};

  EXPECT_EQ(AtlasNetMovementPredictStep(&state, &input, 1, &predicted), ATLAS_NET_ERR_INVAL);
}

TEST(NetClientAbi, MovementPredictStepRejectsInvalidClientDt) {
  AtlasMovementStateFrame state{};
  state.direction_z = 1.0f;
  state.flags = 1u;

  AtlasMovementInputFrame input{};
  input.client_dt_ms = 0;
  AtlasMovementStateFrame predicted{};

  EXPECT_EQ(AtlasNetMovementPredictStep(&state, &input, 1, &predicted), ATLAS_NET_ERR_INVAL);
}
