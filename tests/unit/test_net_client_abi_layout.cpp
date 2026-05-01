#include <cstddef>

#include <gtest/gtest.h>

#include "net_client/client_api.h"

static_assert(sizeof(AtlasNetCallbacks) == 10 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_disconnect)         == 0 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_player_base_create) == 1 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_player_cell_create) == 2 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_reset_entities)     == 3 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_entity_enter)       == 4 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_entity_leave)       == 5 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_entity_position)    == 6 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_entity_property)    == 7 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_forced_position)    == 8 * sizeof(void*));
static_assert(offsetof(AtlasNetCallbacks, on_rpc)                == 9 * sizeof(void*));

static_assert(sizeof(AtlasNetStats) == 24);
static_assert(offsetof(AtlasNetStats, rtt_ms)          == 0);
static_assert(offsetof(AtlasNetStats, bytes_sent)      == 4);
static_assert(offsetof(AtlasNetStats, bytes_recv)      == 8);
static_assert(offsetof(AtlasNetStats, packets_lost)    == 12);
static_assert(offsetof(AtlasNetStats, send_queue_size) == 16);
static_assert(offsetof(AtlasNetStats, loss_rate)       == 20);

TEST(NetClientAbi, VersionMatchesHeaderConstant) {
  EXPECT_EQ(AtlasNetGetAbiVersion(), ATLAS_NET_ABI_VERSION);
}

TEST(NetClientAbi, CreateRejectsMajorMismatch) {
  constexpr uint32_t kBadMajor = 0x02000000u;
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

TEST(NetClientAbi, NullCtxReturnsInvalArg) {
  EXPECT_EQ(AtlasNetPoll(nullptr), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetDisconnect(nullptr, ATLAS_DISCONNECT_USER), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSendBaseRpc(nullptr, 0, 0, nullptr, 0), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSendCellRpc(nullptr, 0, 0, nullptr, 0), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetSetCallbacks(nullptr, nullptr), ATLAS_NET_ERR_INVAL);
  EXPECT_EQ(AtlasNetGetStats(nullptr, nullptr), ATLAS_NET_ERR_INVAL);
}

TEST(NetClientAbi, StateMatrixRejectsIllegalCalls) {
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);

  EXPECT_EQ(AtlasNetAuthenticate(ctx, [](void*, uint8_t, uint32_t, uint16_t,
                                         const char*) {},
                                 nullptr),
            ATLAS_NET_ERR_BUSY);
  EXPECT_EQ(AtlasNetSendBaseRpc(ctx, 1, 1, nullptr, 0), ATLAS_NET_ERR_NOCONN);
  EXPECT_EQ(AtlasNetSendCellRpc(ctx, 1, 1, nullptr, 0), ATLAS_NET_ERR_NOCONN);

  AtlasNetDestroy(ctx);
}
